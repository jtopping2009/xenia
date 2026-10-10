/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SENDSPIN_SENDSPIN_CLIENT_H_
#define XENIA_APU_SENDSPIN_SENDSPIN_CLIENT_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/apu/sendspin/noise_session.h"
#include "xenia/apu/sendspin/sendspin_time_filter.h"
#include "xenia/apu/sendspin/websocket_client.h"

namespace xe {
class Memory;
namespace apu {
class AudioSystem;
namespace sendspin {

// Sendspin (Music Assistant's native player protocol) client. Being built up
// milestone by milestone per the project plan - see that plan for the full
// sequence.
//
// Covers milestone 1 (WebSocket transport + cleartext client/init <->
// server/init), milestone 2 (the Noise_KKpsk2_25519_ChaChaPoly_SHA256
// handshake, using the spec's public Sentinel PSK since this player isn't
// paired yet), and milestone 3 (server/hello -> client/hello ->
// server/activate over the now-encrypted transport, declaring Xenia as a
// player@v1 with a PCM-only format list), milestone 4 (the client/time <->
// server/time NTP-style exchange loop, fed into a Kalman time filter, gating
// client/state's available:true on it converging - see
// sendspin_time_filter.h), and milestone 5 (receiving stream/start and the
// binary audio-chunk frames it precedes, translating each chunk's server
// timestamp to local playback time via the time filter, and feeding the PCM
// into Xenia's AudioSystem - same RegisterClient/SubmitFrame pattern as
// title_music_player.cc).
//
// Milestone 5's audio pacing (spacing SubmitFrame calls out in real time)
// runs on its own thread, separate from the network receive/dispatch loop.
// The first version paced inline in the receive loop and that starved
// everything else on the connection: a stream/end (or a server/time reply)
// sitting behind even a few seconds of already-buffered, not-yet-due audio
// chunks had to wait out their entire real-time playback duration before the
// loop could get back around to reading it - observed as several seconds of
// lag between hitting pause in Music Assistant and Xenia actually going
// quiet, and as the time filter visibly diverging (see HandleServerTimeMessage
// timestamps) the longer a track played. Splitting the two means the network
// thread only ever does non-blocking work (decrypt, dispatch, enqueue).
class SendspinClient {
 public:
  SendspinClient() = default;
  // Safety net: joins the playback thread (if one is still running) and
  // releases the audio client/guest buffer (if still registered) - every
  // successful/failed return path in RunSessionLoop already does this
  // itself via TeardownAudioClient, but a stack-local SendspinClient going
  // out of scope any other way (an early return added later, an exception)
  // must not do so with a joinable std::thread still referencing this
  // object, which would call std::terminate.
  ~SendspinClient();

  // Connects, completes the cleartext exchange, the Noise handshake, and the
  // hello/activate exchange, then runs a genuinely persistent session:
  // converges the time filter, plays whatever Music Assistant streams to
  // this player through audio_system, and keeps going across any number of
  // stream/start -> stream/end cycles (track changes, pauses) for as long as
  // the connection stays healthy. Only returns (false) on a real failure -
  // a transport error, a malformed message, or the connection closing -
  // callers that want to stay connected indefinitely (see
  // SendspinPlayerService) should just reconnect when it does.
  bool ConnectAndProbe(const std::string& host, uint16_t port,
                       apu::AudioSystem* audio_system);

  const std::array<uint8_t, 32>& send_key() const { return send_key_; }
  const std::array<uint8_t, 32>& receive_key() const { return receive_key_; }
  const SendspinTimeFilter& time_filter() const { return time_filter_; }

 private:
  // Loads the persisted X25519 identity keypair from the sendspin_identity_key
  // cvar, generating and persisting a new one on first use.
  void EnsureIdentityKey();

  // Post-handshake transport framing (see Sendspin's messaging.md): a JSON
  // control message is a WebSocket BINARY frame whose Noise-decrypted
  // plaintext is a single 0x00 type byte followed by UTF-8 JSON. Only that
  // single-frame, unfragmented case is handled - sufficient for the small
  // control messages milestones 3-4 exchange.
  bool SendEncryptedJson(const std::string& json_text);
  bool ReceiveEncryptedJson(std::string* out_json_text);
  // Shared by ReceiveEncryptedJson and the milestone 5 dispatch loop: reads
  // one WebSocket frame and Noise-decrypts it, handing back the raw
  // plaintext (still type-byte-prefixed - 0 for JSON, 4 for an audio chunk;
  // see Sendspin's messaging.md) without interpreting it further.
  bool ReceiveEncryptedFrame(std::vector<uint8_t>* out_plaintext,
                            std::chrono::milliseconds timeout);

  // Milestones 4+5: a single unified dispatch loop covering both, since the
  // real protocol doesn't actually separate them - the server can (and, in
  // testing, reliably does) send stream/start the moment it activates us,
  // before our client/time <-> server/time exchange has necessarily
  // converged. An earlier version ran these as two sequential phases (a
  // fixed-iteration time-sync loop, then a separate stream-waiting loop);
  // that broke the instant a real MA server started streaming quickly,
  // because the time-sync phase's "skip anything that isn't server/time"
  // logic only understood other JSON messages, not the raw binary
  // audio-chunk frames that arrive once playback has actually started - it
  // treated one as a hard failure and aborted the whole connection.
  //
  // Sends client/time on aiosendspin's adaptive schedule (fast while
  // unsynchronized, tapering as the filter's error estimate tightens),
  // dispatches every incoming message via its type (server/time feeds
  // time_filter_ and, the first time it converges, sends client/state
  // {available:true}; stream/start registers the audio client and starts the
  // playback thread; stream/clear and stream/end are forwarded to it -
  // stream/end tears the current audio client down but does NOT end the
  // session, since it fires at every ordinary track change; anything else is
  // logged and ignored). Loops indefinitely - the only way out is a real
  // failure (returns false).
  bool RunSessionLoop();
  bool SendClientTimeSyncPing();
  bool HandleServerTimeMessage(const std::string& json_text);
  bool HandleStreamStartMessage(const std::string& json_text);

  // Network-thread side of an audio chunk: translates its server timestamp
  // to local time (time_filter_ is only ever touched from this thread) and
  // hands the samples off to the playback thread. Never blocks.
  void EnqueueAudioChunk(int64_t server_timestamp_us, const uint8_t* pcm_data,
                        size_t pcm_size);
  // Starts the playback thread if one isn't already running for this stream.
  void StartPlaybackThreadIfNeeded();
  // Signals the playback thread to discard whatever it's buffered (a
  // stream/clear - e.g. for a seek) without stopping it.
  void ClearPlaybackQueue();
  // Signals the playback thread to drain and exit, then joins it. Safe to
  // call even if no thread is running.
  void StopPlaybackThread();
  // Runs on playback_thread_: paces SubmitFrame calls to each chunk's
  // translated local playback time - the only thing that may legitimately
  // block for real time, now isolated from network I/O.
  void PlaybackThreadMain();

  void TeardownAudioClient();

  static int64_t NowMicros();

  WebSocketClient ws_;
  std::array<uint8_t, 32> identity_priv_{};
  std::array<uint8_t, 32> identity_pub_{};
  std::array<uint8_t, 32> send_key_{};
  std::array<uint8_t, 32> receive_key_{};
  uint64_t send_nonce_ = 0;
  uint64_t receive_nonce_ = 0;
  SendspinTimeFilter time_filter_;
  int64_t pending_time_sync_t1_us_ = 0;

  // A chunk of interleaved stereo PCM, already translated to a local
  // (steady_clock) target playback time by the network thread, waiting for
  // the playback thread to pace it out via SubmitFrame.
  struct PendingAudioChunk {
    int64_t local_time_us = 0;
    std::vector<int16_t> samples;  // Interleaved L/R, int16.
  };

  // Milestone 5 playback state. audio_client_index_/audio_client_registered_/
  // guest_frame_buffer_/stream_sample_rate_/stream_channels_/
  // stream_bit_depth_ are only touched from the network thread (set up by
  // HandleStreamStartMessage before the playback thread starts, torn down by
  // TeardownAudioClient after it's joined) - never concurrently with
  // PlaybackThreadMain. audio_system_/memory_ are set once and read-only
  // from both threads afterward.
  apu::AudioSystem* audio_system_ = nullptr;
  Memory* memory_ = nullptr;
  size_t audio_client_index_ = 0;
  bool audio_client_registered_ = false;
  uint32_t guest_frame_buffer_ = 0;
  bool stream_active_ = false;
  int stream_sample_rate_ = 0;
  int stream_channels_ = 0;
  int stream_bit_depth_ = 0;

  std::thread playback_thread_;
  std::mutex playback_queue_mutex_;
  std::condition_variable playback_queue_cv_;
  std::deque<PendingAudioChunk> playback_queue_;
  std::atomic<bool> playback_stop_requested_{false};
  std::atomic<bool> playback_clear_requested_{false};
};

}  // namespace sendspin
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SENDSPIN_SENDSPIN_CLIENT_H_
