/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sendspin/sendspin_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "xenia/apu/audio_system.h"
#include "xenia/apu/sendspin/noise_crypto.h"
#include "xenia/apu/sendspin/sendspin_flags.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace {
// Xenia's AudioSystem consumes fixed-size, 6-channel planar float32 frames
// (front-left/front-right in channels 0/1, the rest silent for a stereo
// source) at a 48kHz frame rate, byte-swapped for the PowerPC guest - the
// same layout xenia/apu/title_music_player.cc submits decoded XMP songs in.
constexpr uint32_t kAudioFrameChannels = 6;
constexpr uint32_t kAudioChannelSamples = 256;
constexpr uint32_t kAudioFrameSamples = kAudioFrameChannels * kAudioChannelSamples;
constexpr uint32_t kAudioFrameRate = 48000;
}  // namespace

namespace xe {
namespace apu {
namespace sendspin {

namespace {

// Published constant PSK used when a client isn't paired yet; public, so it
// authenticates nothing on its own - it just gets us to encrypted transport
// so the player can show up (unpaired) on the server. Matches aiosendspin's
// noise/constants.py: sha256(b"sendspin-sentinel-psk-v1").
std::array<uint8_t, 32> SentinelPsk() {
  static const char kLabel[] = "sendspin-sentinel-psk-v1";
  return Sha256(reinterpret_cast<const uint8_t*>(kLabel), sizeof(kLabel) - 1);
}

std::string ToHex(const uint8_t* data, size_t length) {
  static const char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(kHexDigits[data[i] >> 4]);
    out.push_back(kHexDigits[data[i] & 0xF]);
  }
  return out;
}

}  // namespace

SendspinClient::~SendspinClient() { TeardownAudioClient(); }

void SendspinClient::EnsureIdentityKey() {
  if (cvars::sendspin_identity_key.empty()) {
    FillRandomBytes(identity_priv_.data(), identity_priv_.size());
    PersistSendspinIdentityKey(
        Base64Encode(identity_priv_.data(), identity_priv_.size()));
    XELOGI("SendspinClient: generated and persisted a new identity key");
  } else {
    std::vector<uint8_t> decoded;
    if (!Base64Decode(cvars::sendspin_identity_key, &decoded) ||
        decoded.size() != identity_priv_.size()) {
      XELOGE(
          "SendspinClient: sendspin_identity_key in config is malformed - "
          "generating a new one (this player will look unpaired again)");
      FillRandomBytes(identity_priv_.data(), identity_priv_.size());
      PersistSendspinIdentityKey(
          Base64Encode(identity_priv_.data(), identity_priv_.size()));
    } else {
      std::memcpy(identity_priv_.data(), decoded.data(), decoded.size());
    }
  }
  identity_pub_ = X25519PublicKey(identity_priv_);
}

bool SendspinClient::ConnectAndProbe(const std::string& host, uint16_t port,
                                     apu::AudioSystem* audio_system) {
  audio_system_ = audio_system;
  memory_ = audio_system->memory();

  EnsureIdentityKey();

  if (!ws_.Connect(host, port, "/sendspin")) {
    XELOGE("SendspinClient: WebSocket handshake to {}:{}/sendspin failed",
           host, port);
    return false;
  }
  XELOGI("SendspinClient: WebSocket connected to {}:{}/sendspin", host, port);

  std::string client_id =
      Base64UrlEncodeNoPadding(identity_pub_.data(), identity_pub_.size());

  rapidjson::Document doc;
  doc.SetObject();
  auto& allocator = doc.GetAllocator();
  doc.AddMember("type", "client/init", allocator);
  rapidjson::Value payload_obj(rapidjson::kObjectType);
  payload_obj.AddMember(
      "client_id", rapidjson::Value(client_id.c_str(), allocator), allocator);
  payload_obj.AddMember("version", 1, allocator);
  payload_obj.AddMember("suite", "25519_ChaChaPoly_SHA256", allocator);
  doc.AddMember("payload", payload_obj, allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  std::string client_init_text = buffer.GetString();

  XELOGI("SendspinClient: sending client/init: {}", client_init_text);
  if (!ws_.SendText(client_init_text)) {
    XELOGE("SendspinClient: failed to send client/init");
    return false;
  }

  std::vector<uint8_t> frame_payload;
  bool is_binary = false;
  if (!ws_.ReceiveFrame(&frame_payload, &is_binary,
                       std::chrono::milliseconds(10000))) {
    XELOGE("SendspinClient: no server/init received (timeout or closed)");
    return false;
  }
  if (is_binary) {
    XELOGE("SendspinClient: expected server/init (TEXT), got a BINARY frame");
    return false;
  }
  std::string server_init_text(frame_payload.begin(), frame_payload.end());
  XELOGI("SendspinClient: received server/init: {}", server_init_text);

  rapidjson::Document server_init_doc;
  if (server_init_doc.Parse(server_init_text.c_str()).HasParseError() ||
      !server_init_doc.IsObject() || !server_init_doc.HasMember("type") ||
      server_init_doc["type"] != "server/init" ||
      !server_init_doc.HasMember("payload") ||
      !server_init_doc["payload"].IsObject() ||
      !server_init_doc["payload"].HasMember("server_id") ||
      !server_init_doc["payload"]["server_id"].IsString()) {
    XELOGE("SendspinClient: malformed or unexpected server/init");
    return false;
  }
  std::string server_id = server_init_doc["payload"]["server_id"].GetString();

  std::vector<uint8_t> server_static_pub_bytes;
  if (!Base64UrlDecodeNoPadding(server_id, &server_static_pub_bytes) ||
      server_static_pub_bytes.size() != 32) {
    XELOGE("SendspinClient: server_id did not decode to a 32-byte key");
    return false;
  }
  std::array<uint8_t, 32> server_static_pub;
  std::memcpy(server_static_pub.data(), server_static_pub_bytes.data(), 32);

  std::vector<uint8_t> prologue;
  prologue.insert(prologue.end(), client_init_text.begin(),
                  client_init_text.end());
  prologue.insert(prologue.end(), server_init_text.begin(),
                  server_init_text.end());

  NoiseKkPsk2ResponderSession session(identity_priv_, identity_pub_,
                                      server_static_pub, prologue.data(),
                                      prologue.size());

  frame_payload.clear();
  if (!ws_.ReceiveFrame(&frame_payload, &is_binary,
                       std::chrono::milliseconds(10000))) {
    XELOGE(
        "SendspinClient: no noise/handshake (message 1) received (timeout "
        "or closed)");
    return false;
  }
  if (is_binary) {
    XELOGE(
        "SendspinClient: expected noise/handshake (TEXT), got a BINARY "
        "frame");
    return false;
  }
  std::string handshake1_text(frame_payload.begin(), frame_payload.end());

  rapidjson::Document handshake1_doc;
  if (handshake1_doc.Parse(handshake1_text.c_str()).HasParseError() ||
      !handshake1_doc.IsObject() || !handshake1_doc.HasMember("type") ||
      handshake1_doc["type"] != "noise/handshake" ||
      !handshake1_doc.HasMember("payload") ||
      !handshake1_doc["payload"].IsObject() ||
      !handshake1_doc["payload"].HasMember("data") ||
      !handshake1_doc["payload"]["data"].IsString()) {
    XELOGE("SendspinClient: malformed or unexpected noise/handshake message");
    return false;
  }
  std::string message1_b64 = handshake1_doc["payload"]["data"].GetString();
  std::vector<uint8_t> message1_bytes;
  if (!Base64UrlDecodeNoPadding(message1_b64, &message1_bytes)) {
    XELOGE("SendspinClient: noise/handshake payload.data is not valid base64url");
    return false;
  }

  std::vector<uint8_t> message1_payload;
  if (!session.ReadMessage1(message1_bytes.data(), message1_bytes.size(),
                            &message1_payload)) {
    XELOGE(
        "SendspinClient: Noise message 1 failed to authenticate - static "
        "key mismatch or corrupted message");
    return false;
  }
  std::string message1_payload_text(message1_payload.begin(),
                                    message1_payload.end());
  XELOGI("SendspinClient: Noise message 1 verified, payload: {}",
        message1_payload_text);

  // We aren't paired yet (milestone 7), so we always answer under the
  // public Sentinel PSK regardless of the psk_id/psk_category the server
  // asked for - this is exactly the fallback a real client takes when it
  // doesn't recognize the requested credential.
  session.SetPsk(SentinelPsk());

  static const char kEmptyObject[] = "{}";
  auto message2_bytes = session.WriteMessage2(
      reinterpret_cast<const uint8_t*>(kEmptyObject),
      sizeof(kEmptyObject) - 1);
  std::string message2_b64 =
      Base64UrlEncodeNoPadding(message2_bytes.data(), message2_bytes.size());

  rapidjson::Document handshake2_doc;
  handshake2_doc.SetObject();
  auto& allocator2 = handshake2_doc.GetAllocator();
  handshake2_doc.AddMember("type", "noise/handshake", allocator2);
  rapidjson::Value handshake2_payload(rapidjson::kObjectType);
  handshake2_payload.AddMember(
      "data", rapidjson::Value(message2_b64.c_str(), allocator2), allocator2);
  handshake2_doc.AddMember("payload", handshake2_payload, allocator2);
  rapidjson::StringBuffer buffer2;
  rapidjson::Writer<rapidjson::StringBuffer> writer2(buffer2);
  handshake2_doc.Accept(writer2);
  std::string handshake2_text = buffer2.GetString();

  if (!ws_.SendText(handshake2_text)) {
    XELOGE("SendspinClient: failed to send Noise message 2");
    return false;
  }

  auto keys = session.Split();
  send_key_ = keys.send_key;
  receive_key_ = keys.receive_key;
  send_nonce_ = 0;
  receive_nonce_ = 0;

  XELOGI(
      "SendspinClient: Noise handshake complete under the Sentinel PSK - "
      "transport is encrypted. server_id={} handshake_hash={}", server_id,
      ToHex(session.handshake_hash().data(), 8));

  // The server speaks first on the encrypted transport.
  std::string server_hello_text;
  if (!ReceiveEncryptedJson(&server_hello_text)) {
    XELOGE("SendspinClient: failed to receive server/hello");
    return false;
  }
  rapidjson::Document server_hello_doc;
  if (server_hello_doc.Parse(server_hello_text.c_str()).HasParseError() ||
      !server_hello_doc.IsObject() || !server_hello_doc.HasMember("type") ||
      server_hello_doc["type"] != "server/hello") {
    XELOGE("SendspinClient: expected server/hello, got: {}", server_hello_text);
    return false;
  }
  XELOGI("SendspinClient: received server/hello: {}", server_hello_text);

  rapidjson::Document hello_doc;
  hello_doc.SetObject();
  auto& hello_allocator = hello_doc.GetAllocator();
  hello_doc.AddMember("type", "client/hello", hello_allocator);
  rapidjson::Value hello_payload(rapidjson::kObjectType);
  hello_payload.AddMember("name", "Xenia", hello_allocator);
  rapidjson::Value supported_roles(rapidjson::kArrayType);
  supported_roles.PushBack("player@v1", hello_allocator);
  hello_payload.AddMember("supported_roles", supported_roles, hello_allocator);
  rapidjson::Value player_support(rapidjson::kObjectType);
  rapidjson::Value supported_formats(rapidjson::kArrayType);
  rapidjson::Value pcm_format(rapidjson::kObjectType);
  // PCM-only, so no Opus/FLAC decoder is needed on the receive path (milestone 5).
  pcm_format.AddMember("codec", "pcm", hello_allocator);
  pcm_format.AddMember("channels", 2, hello_allocator);
  pcm_format.AddMember("sample_rate", 48000, hello_allocator);
  pcm_format.AddMember("bit_depth", 16, hello_allocator);
  supported_formats.PushBack(pcm_format, hello_allocator);
  player_support.AddMember("supported_formats", supported_formats,
                           hello_allocator);
  player_support.AddMember("buffer_capacity", 1048576, hello_allocator);
  // Required (not optional) on the server version actually deployed
  // (aiosendspin 9.1.1, pinned by music-assistant-server) - confirmed via a
  // local repro against the real wheel after the server logged a generic
  // "invalid value" for the whole client/hello payload with no further
  // detail. Restricted to volume/mute by ClientHelloPlayerSupport.__post_init__.
  rapidjson::Value supported_commands(rapidjson::kArrayType);
  supported_commands.PushBack("volume", hello_allocator);
  supported_commands.PushBack("mute", hello_allocator);
  player_support.AddMember("supported_commands", supported_commands,
                           hello_allocator);
  hello_payload.AddMember("player@v1_support", player_support, hello_allocator);
  rapidjson::Value unpaired_access(rapidjson::kObjectType);
  unpaired_access.AddMember("enabled", true, hello_allocator);
  hello_payload.AddMember("unpaired_access", unpaired_access, hello_allocator);
  hello_doc.AddMember("payload", hello_payload, hello_allocator);
  rapidjson::StringBuffer hello_buffer;
  rapidjson::Writer<rapidjson::StringBuffer> hello_writer(hello_buffer);
  hello_doc.Accept(hello_writer);
  std::string client_hello_text = hello_buffer.GetString();

  XELOGI("SendspinClient: sending client/hello: {}", client_hello_text);
  if (!SendEncryptedJson(client_hello_text)) {
    XELOGE("SendspinClient: failed to send client/hello");
    return false;
  }

  std::string server_activate_text;
  if (!ReceiveEncryptedJson(&server_activate_text)) {
    XELOGE("SendspinClient: failed to receive server/activate");
    return false;
  }
  rapidjson::Document activate_doc;
  if (activate_doc.Parse(server_activate_text.c_str()).HasParseError() ||
      !activate_doc.IsObject() || !activate_doc.HasMember("type") ||
      activate_doc["type"] != "server/activate") {
    XELOGE("SendspinClient: expected server/activate, got: {}",
          server_activate_text);
    return false;
  }
  XELOGI("SendspinClient: received server/activate: {}", server_activate_text);
  XELOGI("SendspinClient: milestone 3 complete - player@v1 activated");

  return RunSessionLoop();
}

int64_t SendspinClient::NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool SendspinClient::SendEncryptedJson(const std::string& json_text) {
  std::vector<uint8_t> plaintext;
  plaintext.reserve(1 + json_text.size());
  plaintext.push_back(0);  // MSG_TYPE_JSON_BODY
  plaintext.insert(plaintext.end(), json_text.begin(), json_text.end());

  static const uint8_t kNoAd[1] = {0};
  auto ciphertext = Chacha20Poly1305IetfEncrypt(
      send_key_, send_nonce_, kNoAd, 0, plaintext.data(), plaintext.size());
  ++send_nonce_;

  std::vector<uint8_t> frame(ciphertext.begin(), ciphertext.end());
  return ws_.SendBinary(frame);
}

bool SendspinClient::ReceiveEncryptedJson(std::string* out_json_text) {
  std::vector<uint8_t> plaintext;
  if (!ReceiveEncryptedFrame(&plaintext, std::chrono::milliseconds(10000))) {
    XELOGE("SendspinClient: no encrypted message received (timeout or closed)");
    return false;
  }
  if (plaintext.empty()) {
    XELOGE("SendspinClient: empty plaintext after decrypting transport frame");
    return false;
  }
  uint8_t type_byte = plaintext[0];
  if (type_byte != 0) {
    XELOGE(
        "SendspinClient: expected a JSON body (type 0), got transport type "
        "{} (fragmentation isn't handled yet)",
        type_byte);
    return false;
  }
  out_json_text->assign(plaintext.begin() + 1, plaintext.end());
  return true;
}

bool SendspinClient::ReceiveEncryptedFrame(std::vector<uint8_t>* out_plaintext,
                                           std::chrono::milliseconds timeout) {
  std::vector<uint8_t> frame_payload;
  bool is_binary = false;
  if (!ws_.ReceiveFrame(&frame_payload, &is_binary, timeout)) {
    return false;
  }
  if (!is_binary) {
    XELOGE(
        "SendspinClient: expected an encrypted BINARY frame, got TEXT: {}",
        std::string(frame_payload.begin(), frame_payload.end()));
    return false;
  }

  static const uint8_t kNoAd[1] = {0};
  if (!Chacha20Poly1305IetfDecrypt(receive_key_, receive_nonce_, kNoAd, 0,
                                   frame_payload.data(), frame_payload.size(),
                                   out_plaintext)) {
    XELOGE(
        "SendspinClient: transport frame failed to authenticate (nonce {})",
        receive_nonce_);
    return false;
  }
  ++receive_nonce_;
  return true;
}

bool SendspinClient::SendClientTimeSyncPing() {
  pending_time_sync_t1_us_ = NowMicros();

  rapidjson::Document time_doc;
  time_doc.SetObject();
  auto& time_allocator = time_doc.GetAllocator();
  time_doc.AddMember("type", "client/time", time_allocator);
  rapidjson::Value time_payload(rapidjson::kObjectType);
  time_payload.AddMember("client_transmitted", pending_time_sync_t1_us_,
                         time_allocator);
  time_doc.AddMember("payload", time_payload, time_allocator);
  rapidjson::StringBuffer time_buffer;
  rapidjson::Writer<rapidjson::StringBuffer> time_writer(time_buffer);
  time_doc.Accept(time_writer);

  if (!SendEncryptedJson(time_buffer.GetString())) {
    XELOGE("SendspinClient: failed to send client/time refresh");
    return false;
  }
  return true;
}

bool SendspinClient::HandleServerTimeMessage(const std::string& json_text) {
  int64_t t4_client_received = NowMicros();

  rapidjson::Document doc;
  if (doc.Parse(json_text.c_str()).HasParseError() || !doc.IsObject() ||
      !doc.HasMember("payload") || !doc["payload"].IsObject() ||
      !doc["payload"].HasMember("server_received") ||
      !doc["payload"]["server_received"].IsInt64() ||
      !doc["payload"].HasMember("server_transmitted") ||
      !doc["payload"]["server_transmitted"].IsInt64()) {
    XELOGE("SendspinClient: malformed server/time: {}", json_text);
    return true;  // Not fatal - just skip this refresh.
  }
  int64_t server_received = doc["payload"]["server_received"].GetInt64();
  int64_t server_transmitted = doc["payload"]["server_transmitted"].GetInt64();

  int64_t offset = static_cast<int64_t>(std::llround(
      ((server_received - pending_time_sync_t1_us_) +
       (server_transmitted - t4_client_received)) /
      2.0));
  int64_t max_error = static_cast<int64_t>(std::llround(
      ((t4_client_received - pending_time_sync_t1_us_) -
       (server_transmitted - server_received)) /
      2.0));
  time_filter_.Update(offset, max_error, t4_client_received);

  XELOGI(
      "SendspinClient: time sync refresh: filter_offset={:.1f}us "
      "filter_error={}us",
      time_filter_.offset(), time_filter_.error());
  return true;
}

bool SendspinClient::HandleStreamStartMessage(const std::string& json_text) {
  rapidjson::Document doc;
  if (doc.Parse(json_text.c_str()).HasParseError() || !doc.IsObject() ||
      !doc.HasMember("payload") || !doc["payload"].IsObject()) {
    XELOGE("SendspinClient: malformed stream/start: {}", json_text);
    return false;
  }
  const auto& payload = doc["payload"];
  if (!payload.HasMember("player") || !payload["player"].IsObject()) {
    // stream/start without a player object is for another role
    // (artwork/visualizer) - nothing for us to do.
    XELOGI("SendspinClient: stream/start without a player payload - ignoring");
    return true;
  }
  const auto& player = payload["player"];
  std::string codec = player.HasMember("codec") && player["codec"].IsString()
                          ? player["codec"].GetString()
                          : std::string();
  int sample_rate = player.HasMember("sample_rate") &&
                            player["sample_rate"].IsInt()
                        ? player["sample_rate"].GetInt()
                        : 0;
  int channels = player.HasMember("channels") && player["channels"].IsInt()
                     ? player["channels"].GetInt()
                     : 0;
  int bit_depth = player.HasMember("bit_depth") && player["bit_depth"].IsInt()
                      ? player["bit_depth"].GetInt()
                      : 0;

  // Only the exact format declared in client/hello is handled - the server
  // has no reason to send anything else since that's the only format we
  // advertised support for.
  if (codec != "pcm" || sample_rate != int(kAudioFrameRate) ||
      channels != 2 || bit_depth != 16) {
    XELOGE(
        "SendspinClient: stream/start declared an unsupported format "
        "(codec={} sample_rate={} channels={} bit_depth={})",
        codec, sample_rate, channels, bit_depth);
    return false;
  }

  stream_sample_rate_ = sample_rate;
  stream_channels_ = channels;
  stream_bit_depth_ = bit_depth;
  stream_active_ = true;

  if (!audio_client_registered_) {
    X_STATUS register_status =
        audio_system_->RegisterClient(0, 0, &audio_client_index_);
    if (XFAILED(register_status)) {
      XELOGE("SendspinClient: failed to register audio client");
      stream_active_ = false;
      return false;
    }
    audio_client_registered_ = true;
  }
  if (!guest_frame_buffer_) {
    guest_frame_buffer_ =
        memory_->SystemHeapAlloc(kAudioFrameSamples * sizeof(float));
  }

  StartPlaybackThreadIfNeeded();

  XELOGI(
      "SendspinClient: stream/start - playing pcm {}Hz {}ch {}-bit",
      sample_rate, channels, bit_depth);
  return true;
}

void SendspinClient::EnqueueAudioChunk(int64_t server_timestamp_us,
                                       const uint8_t* pcm_data,
                                       size_t pcm_size) {
  if (!stream_active_ || stream_channels_ != 2 || stream_bit_depth_ != 16) {
    return;
  }
  size_t sample_pairs = pcm_size / 4;  // 2 channels * 2 bytes/sample.
  if (sample_pairs == 0) {
    return;
  }

  // time_filter_ is only ever touched from this (the network) thread, so
  // this translation happens here rather than on the playback thread - it
  // needs no locking against RunTimeSyncLoop/HandleServerTimeMessage.
  int64_t local_time_us = time_filter_.ComputeClientTime(server_timestamp_us);

  // PCM on the wire is little-endian (the reference server implementation
  // runs on x86 and copies samples through without any byte-order handling
  // of its own) - a plain reinterpret is correct on our little-endian host.
  const int16_t* samples = reinterpret_cast<const int16_t*>(pcm_data);

  PendingAudioChunk chunk;
  chunk.local_time_us = local_time_us;
  chunk.samples.assign(samples, samples + sample_pairs * 2);

  {
    std::lock_guard<std::mutex> lock(playback_queue_mutex_);
    playback_queue_.push_back(std::move(chunk));
  }
  playback_queue_cv_.notify_one();
}

void SendspinClient::StartPlaybackThreadIfNeeded() {
  if (playback_thread_.joinable()) {
    return;  // Already running - a mid-stream format update, not a new one.
  }
  playback_stop_requested_ = false;
  playback_clear_requested_ = false;
  playback_thread_ = std::thread(&SendspinClient::PlaybackThreadMain, this);
}

void SendspinClient::ClearPlaybackQueue() {
  {
    std::lock_guard<std::mutex> lock(playback_queue_mutex_);
    playback_clear_requested_ = true;
  }
  playback_queue_cv_.notify_one();
}

void SendspinClient::StopPlaybackThread() {
  if (!playback_thread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(playback_queue_mutex_);
    playback_stop_requested_ = true;
    // Drop anything still queued rather than draining it out in real time -
    // a stop should be near-instant, not "instant once whatever's already
    // buffered finishes playing" (that multi-second lag chasing pause in
    // Music Assistant was exactly this, before playback moved to its own
    // thread: a backlog of already-arrived, not-yet-due chunks getting
    // played out in full before anything could react to the stop).
    playback_queue_.clear();
  }
  playback_queue_cv_.notify_one();
  playback_thread_.join();
}

void SendspinClient::PlaybackThreadMain() {
  // Interleaved PCM samples received but not yet forming a full Xenia audio
  // frame, and the predicted local (steady_clock) playback time of the next
  // frame to submit - both local to this thread now, since only it consumes
  // the queue. next_frame_local_time_us advances continuously by one frame's
  // duration per submission, only resynced to a chunk's own timestamp on a
  // large jump (a genuine discontinuity - stream start, seek, underrun -
  // rather than ordinary chunk-to-chunk jitter).
  std::vector<int16_t> pcm_carry;
  double next_frame_local_time_us = -1.0;

  for (;;) {
    PendingAudioChunk chunk;
    {
      std::unique_lock<std::mutex> lock(playback_queue_mutex_);
      playback_queue_cv_.wait(lock, [this] {
        return playback_stop_requested_.load() ||
              playback_clear_requested_.load() || !playback_queue_.empty();
      });
      if (playback_clear_requested_.exchange(false)) {
        playback_queue_.clear();
        pcm_carry.clear();
        next_frame_local_time_us = -1.0;
        XELOGI("SendspinClient: stream/clear - resetting audio buffer");
        continue;
      }
      if (playback_queue_.empty()) {
        // Only remaining reason to have woken up is a stop request with
        // nothing left queued.
        break;
      }
      chunk = std::move(playback_queue_.front());
      playback_queue_.pop_front();
    }

    double carried_duration_us = double(pcm_carry.size() / 2) * 1'000'000.0 /
                                 double(stream_sample_rate_);
    double expected_time_us = next_frame_local_time_us + carried_duration_us;
    if (next_frame_local_time_us < 0.0 ||
        std::fabs(double(chunk.local_time_us) - expected_time_us) > 50000.0) {
      if (next_frame_local_time_us >= 0.0) {
        XELOGI("SendspinClient: audio timeline discontinuity - resyncing");
      }
      pcm_carry.clear();
      next_frame_local_time_us = double(chunk.local_time_us);
    }

    pcm_carry.insert(pcm_carry.end(), chunk.samples.begin(),
                     chunk.samples.end());

    while (pcm_carry.size() >= size_t(kAudioChannelSamples) * 2) {
      // A stop/clear request should interrupt a long run of already-queued
      // frames promptly rather than playing all of it out first.
      if (playback_stop_requested_.load() ||
          playback_clear_requested_.load()) {
        break;
      }
      float* guest_buf = memory_->TranslateVirtual<float*>(guest_frame_buffer_);
      for (uint32_t i = 0; i < kAudioChannelSamples; ++i) {
        float l = std::clamp(pcm_carry[i * 2] / 32768.0f, -1.0f, 1.0f);
        float r = std::clamp(pcm_carry[i * 2 + 1] / 32768.0f, -1.0f, 1.0f);
        guest_buf[0 * kAudioChannelSamples + i] = xe::byte_swap(l);
        guest_buf[1 * kAudioChannelSamples + i] = xe::byte_swap(r);
      }
      for (uint32_t ch = 2; ch < kAudioFrameChannels; ++ch) {
        for (uint32_t i = 0; i < kAudioChannelSamples; ++i) {
          guest_buf[ch * kAudioChannelSamples + i] = 0.0f;
        }
      }
      pcm_carry.erase(pcm_carry.begin(),
                      pcm_carry.begin() + size_t(kAudioChannelSamples) * 2);

      int64_t target_local_us = int64_t(std::llround(next_frame_local_time_us));
      int64_t now_us = NowMicros();
      if (target_local_us > now_us) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(target_local_us - now_us));
      }
      audio_system_->SubmitFrame(audio_client_index_, guest_frame_buffer_);
      next_frame_local_time_us +=
          double(kAudioChannelSamples) * 1'000'000.0 / double(stream_sample_rate_);
    }
  }
}

void SendspinClient::TeardownAudioClient() {
  // Must join the playback thread before unregistering the audio client or
  // freeing the guest buffer - it may still be mid-SubmitFrame against them.
  StopPlaybackThread();
  if (audio_client_registered_) {
    audio_system_->UnregisterClient(audio_client_index_);
    audio_client_registered_ = false;
  }
  if (guest_frame_buffer_) {
    memory_->SystemHeapFree(guest_frame_buffer_);
    guest_frame_buffer_ = 0;
  }
  stream_active_ = false;
}

bool SendspinClient::RunSessionLoop() {
  // Mirrors aiosendspin's _compute_time_sync_interval: fast (0.2s) while
  // unsynchronized, then backing off as the filter's error estimate tightens.
  auto compute_interval_ms = [this]() -> int {
    if (!time_filter_.is_synchronized()) {
      return 200;
    }
    int64_t error_us = time_filter_.error();
    if (error_us < 1000) return 3000;
    if (error_us < 2000) return 1000;
    if (error_us < 5000) return 500;
    return 200;
  };

  if (!SendClientTimeSyncPing()) {
    XELOGE("SendspinClient: failed to send initial client/time");
    return false;
  }
  int64_t last_time_sync_us = NowMicros();
  bool sent_available_state = false;

  XELOGI(
      "SendspinClient: session established - waiting for time sync to "
      "converge and for Music Assistant to start playback");

  // This is now a genuinely persistent session (milestone 6): it runs for as
  // long as the connection stays healthy, across any number of stream/start
  // -> stream/end cycles (track changes, pauses), rather than exiting after
  // one track or a fixed wait. The only way out is a real failure below -
  // SendspinPlayerService reconnects when that happens.
  for (;;) {
    int64_t interval_us = int64_t(compute_interval_ms()) * 1000;
    if (NowMicros() - last_time_sync_us >= interval_us) {
      if (!SendClientTimeSyncPing()) {
        TeardownAudioClient();
        return false;
      }
      last_time_sync_us = NowMicros();
    }

    std::vector<uint8_t> plaintext;
    if (!ReceiveEncryptedFrame(&plaintext, std::chrono::milliseconds(2000))) {
      if (!ws_.is_connected()) {
        XELOGE("SendspinClient: connection closed");
        TeardownAudioClient();
        return false;
      }
      continue;  // Just an idle 2s slice - keep waiting.
    }
    if (plaintext.empty()) {
      continue;
    }

    uint8_t type_byte = plaintext[0];
    if (type_byte == 0) {
      std::string json_text(plaintext.begin() + 1, plaintext.end());
      rapidjson::Document doc;
      if (doc.Parse(json_text.c_str()).HasParseError() || !doc.IsObject() ||
          !doc.HasMember("type") || !doc["type"].IsString()) {
        XELOGE("SendspinClient: malformed JSON control message: {}",
              json_text);
        continue;
      }
      std::string msg_type = doc["type"].GetString();

      if (msg_type == "server/time") {
        HandleServerTimeMessage(json_text);
        if (!sent_available_state && time_filter_.is_synchronized()) {
          rapidjson::Document state_doc;
          state_doc.SetObject();
          auto& state_allocator = state_doc.GetAllocator();
          state_doc.AddMember("type", "client/state", state_allocator);
          rapidjson::Value state_payload(rapidjson::kObjectType);
          state_payload.AddMember("available", true, state_allocator);
          state_doc.AddMember("payload", state_payload, state_allocator);
          rapidjson::StringBuffer state_buffer;
          rapidjson::Writer<rapidjson::StringBuffer> state_writer(state_buffer);
          state_doc.Accept(state_writer);

          XELOGI("SendspinClient: sending client/state: {}",
                state_buffer.GetString());
          if (!SendEncryptedJson(state_buffer.GetString())) {
            XELOGE("SendspinClient: failed to send client/state");
            TeardownAudioClient();
            return false;
          }
          sent_available_state = true;
          XELOGI(
              "SendspinClient: milestone 4 complete - time filter converged "
              "(offset={:.1f}us error={}us), sent client/state "
              "available=true",
              time_filter_.offset(), time_filter_.error());
        }
      } else if (msg_type == "stream/start") {
        if (!HandleStreamStartMessage(json_text)) {
          TeardownAudioClient();
          return false;
        }
      } else if (msg_type == "stream/clear") {
        ClearPlaybackQueue();
      } else if (msg_type == "stream/end") {
        // A stream/end is just "this track (or pause) is over" - it fires
        // at every ordinary track change, not only when the user is done
        // testing. Treating it as terminal (as an earlier version did) tore
        // the whole connection down at the first stream/end, which looked
        // like playback stopping dead the moment a song ended rather than
        // advancing to the next one. Tear down the current audio client and
        // keep looping - a new stream/start for the next track (or a resumed
        // one) is handled the same as the very first.
        XELOGI("SendspinClient: stream/end: {}", json_text);
        TeardownAudioClient();
      } else {
        XELOGI("SendspinClient: ignoring {} while awaiting/playing audio",
              msg_type);
      }
    } else if (type_byte == 4) {
      // AUDIO_CHUNK: [type(1)][timestamp_us big-endian int64(8)][pcm...].
      // Can legitimately arrive before the time filter has converged (the
      // server activates a stream as soon as it admits us, regardless of our
      // own clock sync progress) - EnqueueAudioChunk/ComputeClientTime handle
      // that fine, so there's no need to gate on sent_available_state here.
      if (plaintext.size() < 9 || !stream_active_) {
        continue;
      }
      int64_t server_timestamp_us = 0;
      for (int i = 0; i < 8; ++i) {
        server_timestamp_us = (server_timestamp_us << 8) | plaintext[1 + i];
      }
      EnqueueAudioChunk(server_timestamp_us, plaintext.data() + 9,
                       plaintext.size() - 9);
    } else {
      XELOGI("SendspinClient: ignoring binary message of transport type {}",
            int(type_byte));
    }
  }
}

}  // namespace sendspin
}  // namespace apu
}  // namespace xe
