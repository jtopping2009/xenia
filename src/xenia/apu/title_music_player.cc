/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/title_music_player.h"

#include <algorithm>
#include <cstring>

#include "xenia/apu/audio_system.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/xbox.h"

extern "C" {
#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#include "third_party/FFmpeg/libavutil/mem.h"
#if XE_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Fixed host audio frame format shared by every AudioDriver backend (see
// SDLAudioDriver/XAudio2AudioDriver): 48kHz, 6 channels (5.1: FL, FR, FC,
// LFE, BL, BR), 256 samples per channel per frame, samples laid out as 6
// sequential per-channel blocks (NOT interleaved), each sample a big-endian
// float (guest memory is big-endian PowerPC).
namespace {
constexpr uint32_t kFrameChannels = 6;
constexpr uint32_t kChannelSamples = 256;
constexpr uint32_t kFrameSamples = kFrameChannels * kChannelSamples;
constexpr uint32_t kFrameRate = 48000;

// ASF object GUIDs, as they appear byte-for-byte in the file (Microsoft GUID
// wire format: Data1/Data2/Data3 little-endian, Data4 as-is).
constexpr uint8_t kAsfHeaderObjectGuid[16] = {
    0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
    0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kAsfFilePropertiesObjectGuid[16] = {
    0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
    0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kAsfStreamPropertiesObjectGuid[16] = {
    0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
    0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kAsfAudioMediaGuid[16] = {
    0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
    0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uint8_t kAsfDataObjectGuid[16] = {
    0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
    0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};

bool GuidEquals(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, 16) == 0;
}

uint16_t ReadU16LE(const uint8_t* p) {
  return uint16_t(p[0] | (p[1] << 8));
}
uint32_t ReadU32LE(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
uint64_t ReadU64LE(const uint8_t* p) {
  return uint64_t(ReadU32LE(p)) | (uint64_t(ReadU32LE(p + 4)) << 32);
}

// Cursor-based reader over the in-memory file, mirroring the field-by-field
// parsing AVIOContext would do for a real ASF demuxer, but bounds-checked
// against a fixed buffer instead of a stream.
class ByteCursor {
 public:
  ByteCursor(const uint8_t* data, size_t size, size_t pos)
      : data_(data), size_(size), pos_(pos) {}

  bool ok() const { return !overrun_; }
  size_t pos() const { return pos_; }
  void set_pos(size_t pos) { pos_ = pos; }

  uint8_t U8() {
    if (pos_ + 1 > size_) {
      overrun_ = true;
      return 0;
    }
    return data_[pos_++];
  }
  uint16_t U16() {
    if (pos_ + 2 > size_) {
      overrun_ = true;
      return 0;
    }
    uint16_t v = ReadU16LE(data_ + pos_);
    pos_ += 2;
    return v;
  }
  uint32_t U32() {
    if (pos_ + 4 > size_) {
      overrun_ = true;
      return 0;
    }
    uint32_t v = ReadU32LE(data_ + pos_);
    pos_ += 4;
    return v;
  }
  void Skip(int64_t count) {
    if (count < 0 ? (pos_ < uint64_t(-count)) : (pos_ + count > size_)) {
      overrun_ = true;
      return;
    }
    pos_ += count;
  }
  // Reads a 2-bit "type" field and returns the 0/1/2/4-byte value it selects
  // (matching ASF's ubiquitous Length Type Flags encoding: 0=the provided
  // default, 1=BYTE, 2=WORD, 3=DWORD).
  uint32_t Do2Bits(uint32_t bits, uint32_t default_value) {
    switch (bits & 3) {
      case 1:
        return U8();
      case 2:
        return U16();
      case 3:
        return U32();
      default:
        return default_value;
    }
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_;
  bool overrun_ = false;
};

}  // namespace

namespace xe {
namespace apu {

TitleMusicPlayer::TitleMusicPlayer() = default;

TitleMusicPlayer::~TitleMusicPlayer() {
  Stop();
  ReleasePersistentResources();
}

bool TitleMusicPlayer::ParseAsf(const std::vector<uint8_t>& file_data) {
  if (file_data.size() < 30 ||
      !GuidEquals(file_data.data(), kAsfHeaderObjectGuid)) {
    XELOGE("TitleMusicPlayer: not an ASF file (bad header GUID)");
    return false;
  }
  uint64_t header_size = ReadU64LE(&file_data[16]);
  uint32_t num_header_objects = ReadU32LE(&file_data[24]);
  if (header_size < 30 || header_size > file_data.size()) {
    XELOGE("TitleMusicPlayer: invalid ASF header size {}", header_size);
    return false;
  }

  uint32_t packet_size = 0;
  bool have_stream = false;

  size_t pos = 30;
  for (uint32_t i = 0;
       i < num_header_objects && pos + 24 <= file_data.size(); ++i) {
    const uint8_t* obj = &file_data[pos];
    uint64_t obj_size = ReadU64LE(obj + 16);
    if (obj_size < 24 || pos + obj_size > file_data.size()) {
      XELOGE("TitleMusicPlayer: invalid ASF header sub-object size");
      return false;
    }
    XELOGI(
        "TitleMusicPlayer: header object {}: guid={:02X}{:02X}{:02X}{:02X}-"
        "{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-"
        "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} size={}",
        i, obj[0], obj[1], obj[2], obj[3], obj[4], obj[5], obj[6], obj[7],
        obj[8], obj[9], obj[10], obj[11], obj[12], obj[13], obj[14], obj[15],
        obj_size);
    if (GuidEquals(obj, kAsfFilePropertiesObjectGuid) &&
        obj_size >= 24 + 76) {
      const uint8_t* p = obj + 24;
      packet_size = ReadU32LE(p + 72);  // Maximum Data Packet Size.
    } else if (GuidEquals(obj, kAsfStreamPropertiesObjectGuid)) {
      const uint8_t* p = obj + 24;
      XELOGI(
          "TitleMusicPlayer: stream properties object size={} "
          "stream_type_guid={:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}"
          "-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
          obj_size, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8],
          p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
      if (!have_stream && obj_size >= 24 + 54 &&
          GuidEquals(p, kAsfAudioMediaGuid)) {
        uint32_t type_specific_len = ReadU32LE(p + 40);
        uint16_t flags = ReadU16LE(p + 48);
        const uint8_t* wfx = p + 54;
        size_t wfx_avail = (obj + obj_size) - wfx;
        if (type_specific_len >= 16 && wfx_avail >= 16) {
          stream_info_.stream_number = flags & 0x7F;
          stream_info_.format_tag = ReadU16LE(wfx + 0);
          stream_info_.channels = ReadU16LE(wfx + 2);
          stream_info_.sample_rate = ReadU32LE(wfx + 4);
          stream_info_.avg_bytes_per_sec = ReadU32LE(wfx + 8);
          stream_info_.block_align = ReadU16LE(wfx + 12);
          stream_info_.bits_per_sample = ReadU16LE(wfx + 14);
          if (type_specific_len >= 18 && wfx_avail >= 18) {
            uint16_t cb_size = ReadU16LE(wfx + 16);
            size_t extra_avail = wfx_avail - 18;
            cb_size = uint16_t(std::min<size_t>(cb_size, extra_avail));
            if (cb_size > 0) {
              stream_info_.extra_data.assign(wfx + 18, wfx + 18 + cb_size);
            }
          }
          have_stream = true;
        }
      }
    }
    pos += obj_size;
  }

  if (!have_stream) {
    XELOGE("TitleMusicPlayer: no audio stream found in ASF file");
    return false;
  }
  if (!packet_size) {
    XELOGE("TitleMusicPlayer: no data packet size found in ASF file");
    return false;
  }

  // The Data Object is a top-level sibling of the Header Object, starting
  // immediately after it.
  pos = size_t(header_size);
  if (pos + 50 > file_data.size() ||
      !GuidEquals(&file_data[pos], kAsfDataObjectGuid)) {
    XELOGE("TitleMusicPlayer: expected ASF Data Object not found");
    return false;
  }
  uint64_t total_packets = ReadU64LE(&file_data[pos + 40]);
  size_t packets_start = pos + 50;

  XELOGI(
      "TitleMusicPlayer: ASF stream {} tag=0x{:04X} channels={} rate={} "
      "block_align={} extradata={}B, {} packets of {}B",
      stream_info_.stream_number, stream_info_.format_tag,
      stream_info_.channels, stream_info_.sample_rate,
      stream_info_.block_align, stream_info_.extra_data.size(),
      total_packets, packet_size);

  // A compressed audio block ("media object") can legitimately be split
  // across multiple ASF payloads/packets when a passage needs more bits than
  // fits in one packet (this is more likely during loud/dense passages) -
  // reassemble by Media Object Number/offset rather than assuming each
  // payload is always exactly one complete block.
  std::vector<uint8_t> object_buffer;
  uint32_t object_expected_size = 0;
  bool object_active = false;
  uint32_t split_object_count = 0;

  for (uint64_t packet_index = 0; packet_index < total_packets;
       ++packet_index) {
    size_t packet_start = packets_start + size_t(packet_index) * packet_size;
    if (packet_start + packet_size > file_data.size()) {
      break;
    }
    ByteCursor cur(file_data.data(), file_data.size(), packet_start);
    size_t packet_end = packet_start + packet_size;

    // Error correction flags / packet flags byte, matching FFmpeg's
    // asf_get_packet (see libavformat/asfdec_f.c).
    int rsize = 8;
    uint8_t c = cur.U8();
    if (c & 0x80) {
      if (!(c & 0x60)) {
        cur.U8();  // Standard 3-byte error correction data (2nd byte).
        cur.U8();  // 3rd byte.
        cur.Skip(int64_t(c & 0xF) - 2);
        rsize += (c & 0xF);
      }
      c = cur.U8();
    }
    uint8_t packet_flags = c;
    uint8_t packet_property = cur.U8();

    uint32_t packet_length =
        cur.Do2Bits(packet_flags >> 5, packet_size);
    /* sequence type, unused */ cur.Do2Bits(packet_flags >> 1, 0);
    uint32_t padsize = cur.Do2Bits(packet_flags >> 3, 0);
    cur.U32();  // Send time.
    cur.U16();  // Duration.

    uint8_t packet_segsizetype = 0x80;
    uint32_t packet_segments = 1;
    if (packet_flags & 0x01) {
      packet_segsizetype = cur.U8();
      rsize++;
      packet_segments = packet_segsizetype & 0x3F;
    }

    if (!cur.ok() || packet_length < uint32_t(rsize) + padsize) {
      XELOGW("TitleMusicPlayer: malformed ASF data packet {}, stopping",
             packet_index);
      break;
    }
    uint32_t packet_size_left = packet_length - padsize - rsize;

    bool packet_ok = true;
    for (uint32_t seg = 0; seg < packet_segments && packet_ok; ++seg) {
      size_t seg_header_start = cur.pos();
      uint8_t num = cur.U8();
      uint32_t stream_number = num & 0x7F;
      uint32_t packet_seq =
          cur.Do2Bits(packet_property >> 4, 0);
      uint32_t packet_frag_offset =
          cur.Do2Bits(packet_property >> 2, 0);
      uint32_t packet_replic_size = cur.Do2Bits(packet_property, 0);
      (void)packet_seq;

      if (!cur.ok() || packet_replic_size + 1 > packet_size_left) {
        packet_ok = false;
        break;
      }

      uint32_t media_object_size = 0;
      bool have_media_object_size = false;
      if (packet_replic_size >= 8) {
        media_object_size = cur.U32();
        have_media_object_size = true;
        cur.U32();  // Presentation time.
        cur.Skip(int64_t(packet_replic_size) - 8);
      } else if (packet_replic_size == 1) {
        // Compressed multi-payload sub-fragmentation mode - not produced by
        // typical single-stream background-music encodes; unsupported.
        XELOGW(
            "TitleMusicPlayer: compressed ASF sub-payloads not supported, "
            "stopping");
        packet_ok = false;
        break;
      } else {
        XELOGW("TitleMusicPlayer: unexpected replicated data size {}",
               packet_replic_size);
        packet_ok = false;
        break;
      }
      size_t consumed_so_far = cur.pos() - seg_header_start;
      uint32_t frag_size;
      if (packet_flags & 0x01) {
        frag_size = cur.Do2Bits(packet_segsizetype >> 6, 0);
      } else {
        frag_size = packet_size_left - uint32_t(consumed_so_far);
      }

      if (!cur.ok() || cur.pos() + frag_size > packet_end) {
        packet_ok = false;
        break;
      }

      if (stream_number == stream_info_.stream_number) {
        const uint8_t* payload_data = file_data.data() + cur.pos();
        if (packet_frag_offset == 0) {
          // Starts a new media object - possibly the only fragment, possibly
          // the first of several.
          if (object_active) {
            XELOGW(
                "TitleMusicPlayer: media object abandoned incomplete ({}/{} "
                "bytes)",
                object_buffer.size(), object_expected_size);
          }
          object_buffer.assign(payload_data, payload_data + frag_size);
          object_expected_size =
              have_media_object_size ? media_object_size : frag_size;
          object_active = true;
        } else if (object_active &&
                  packet_frag_offset == object_buffer.size()) {
          // Continues the current media object.
          object_buffer.insert(object_buffer.end(), payload_data,
                               payload_data + frag_size);
          ++split_object_count;
        } else {
          // Out-of-order or missing a fragment - can't safely reassemble.
          XELOGW(
              "TitleMusicPlayer: unexpected media object fragment offset {} "
              "(have {} bytes), dropping",
              packet_frag_offset, object_buffer.size());
          object_active = false;
        }

        if (object_active && object_buffer.size() >= object_expected_size) {
          payloads_.push_back(std::move(object_buffer));
          object_buffer.clear();
          object_active = false;
        }
      }
      cur.Skip(frag_size);
      packet_size_left -= uint32_t(cur.pos() - seg_header_start);
    }
  }

  XELOGI(
      "TitleMusicPlayer: extracted {} audio payloads ({} were reassembled "
      "from multiple fragments)",
      payloads_.size(), split_object_count);
  return !payloads_.empty();
}

bool TitleMusicPlayer::OpenDecoder() {
  AVCodecID codec_id;
  switch (stream_info_.format_tag) {
    case 0x0161:
      codec_id = AV_CODEC_ID_WMAV2;
      break;
    case 0x0160:
      codec_id = AV_CODEC_ID_WMAV1;
      break;
    default:
      XELOGE("TitleMusicPlayer: unsupported WMA format tag 0x{:04X}",
             stream_info_.format_tag);
      return false;
  }

  const AVCodec* codec = avcodec_find_decoder(codec_id);
  if (!codec) {
    XELOGE("TitleMusicPlayer: WMA decoder not found");
    return false;
  }
  av_context_ = avcodec_alloc_context3(codec);
  if (!av_context_) {
    return false;
  }
  av_context_->sample_rate = int(stream_info_.sample_rate);
  av_context_->channels = int(stream_info_.channels);
  av_context_->bit_rate = int64_t(stream_info_.avg_bytes_per_sec) * 8;
  av_context_->block_align = stream_info_.block_align;
  if (!stream_info_.extra_data.empty()) {
    av_context_->extradata = static_cast<uint8_t*>(
        av_mallocz(stream_info_.extra_data.size() +
                   AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(av_context_->extradata, stream_info_.extra_data.data(),
                stream_info_.extra_data.size());
    av_context_->extradata_size = int(stream_info_.extra_data.size());
  }

  if (avcodec_open2(av_context_, codec, nullptr) < 0) {
    XELOGE("TitleMusicPlayer: failed to open WMA decoder");
    return false;
  }

  av_frame_ = av_frame_alloc();
  av_packet_ = av_packet_alloc();
  return av_frame_ && av_packet_;
}

void TitleMusicPlayer::CloseDecoder() {
  if (av_packet_) {
    av_packet_free(&av_packet_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
  if (av_context_) {
    if (avcodec_is_open(av_context_)) {
      avcodec_close(av_context_);
    }
    avcodec_free_context(&av_context_);
  }
}

bool TitleMusicPlayer::Play(kernel::KernelState* kernel_state,
                            AudioSystem* audio_system,
                            const std::u16string& file_path) {
  std::lock_guard<std::recursive_mutex> lock(control_mutex_);
  Stop();

  std::string path_utf8 = xe::to_utf8(file_path);
  auto entry = kernel_state->file_system()->ResolvePath(path_utf8);
  if (!entry) {
    XELOGE("TitleMusicPlayer: song file not found: {}", path_utf8);
    return false;
  }
  vfs::File* file = nullptr;
  if (XFAILED(entry->Open(vfs::FileAccess::kGenericRead, &file))) {
    XELOGE("TitleMusicPlayer: failed to open song file: {}", path_utf8);
    return false;
  }
  std::vector<uint8_t> file_data(entry->size());
  size_t bytes_read = 0;
  X_STATUS read_status =
      file->ReadSync(file_data.data(), file_data.size(), 0, &bytes_read);
  file->Destroy();
  if (XFAILED(read_status)) {
    XELOGE("TitleMusicPlayer: failed to read song file: {}", path_utf8);
    return false;
  }
  file_data.resize(bytes_read);

  if (!ParseAsf(file_data)) {
    return false;
  }
  if (!OpenDecoder()) {
    CloseDecoder();
    return false;
  }

  memory_ = kernel_state->memory();
  audio_system_ = audio_system;
  // The audio client and its guest sample buffer are set up once and held
  // for the life of this player, rather than being torn down and
  // re-requested on every song change (every skip). SystemHeapAlloc/Free
  // round to whole guest pages, and repeatedly punching a fresh
  // allocate/free cycle into that shared heap - right next to the game's
  // own long-lived XamAlloc'd song table - was corrupting the game's
  // storage: freeing our buffer could release page(s) the allocator later
  // handed back out from under a still-live neighboring allocation. A
  // single persistent allocation avoids that churn entirely.
  if (!client_registered_) {
    X_STATUS register_status =
        audio_system_->RegisterClient(0, 0, &client_index_);
    if (XFAILED(register_status)) {
      XELOGE("TitleMusicPlayer: failed to register audio client");
      CloseDecoder();
      return false;
    }
    client_registered_ = true;
  }
  if (!guest_frame_buffer_) {
    guest_frame_buffer_ =
        memory_->SystemHeapAlloc(kFrameSamples * sizeof(float));
  }

  stop_requested_ = false;
  paused_ = false;
  finished_naturally_ = false;
  thread_ = std::thread(&TitleMusicPlayer::ThreadMain, this,
                        std::move(file_data));
  return true;
}

void TitleMusicPlayer::Stop() {
  std::lock_guard<std::recursive_mutex> lock(control_mutex_);
  stop_requested_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  // The audio client and guest_frame_buffer_ are persistent resources (see
  // Play()) and are only torn down in the destructor, not here - Stop() just
  // ends the current song's decode thread so a new one can start.
  CloseDecoder();
  payloads_.clear();
  stream_info_ = AsfStreamInfo();
}

void TitleMusicPlayer::ReleasePersistentResources() {
  std::lock_guard<std::recursive_mutex> lock(control_mutex_);
  if (client_registered_) {
    audio_system_->UnregisterClient(client_index_);
    client_registered_ = false;
  }
  if (guest_frame_buffer_) {
    memory_->SystemHeapFree(guest_frame_buffer_);
    guest_frame_buffer_ = 0;
  }
}

void TitleMusicPlayer::ThreadMain(std::vector<uint8_t> file_data) {
  // Linear-interpolation resampler state, source-rate -> 48kHz.
  double src_pos = 0.0;
  double src_step = double(stream_info_.sample_rate) / double(kFrameRate);
  std::vector<float> src_left;
  std::vector<float> src_right;
  size_t src_consumed = 0;

  std::vector<float> out_left;
  std::vector<float> out_right;
  out_left.reserve(kChannelSamples * 2);
  out_right.reserve(kChannelSamples * 2);

  auto has_enough_source = [&](double up_to_pos) {
    return src_left.size() > src_consumed &&
           double(src_left.size() - 1) >= up_to_pos;
  };

  for (size_t payload_index = 0;
       payload_index < payloads_.size() && !stop_requested_;
       ++payload_index) {
    while (paused_ && !stop_requested_) {
      xe::threading::Sleep(std::chrono::milliseconds(20));
    }
    if (stop_requested_) {
      break;
    }

    const auto& payload = payloads_[payload_index];
    av_packet_->data = const_cast<uint8_t*>(payload.data());
    av_packet_->size = int(payload.size());
    int send_ret = avcodec_send_packet(av_context_, av_packet_);
    av_packet_->data = nullptr;
    av_packet_->size = 0;
    if (send_ret < 0) {
      continue;
    }

    while (true) {
      int recv_ret = avcodec_receive_frame(av_context_, av_frame_);
      if (recv_ret < 0) {
        break;
      }
      int channels = std::max(1, av_context_->channels);
      int samples = av_frame_->nb_samples;

      // Trim any samples already consumed by resampling from the previous
      // frame to keep the source buffers from growing unbounded.
      if (src_consumed > 0) {
        src_left.erase(src_left.begin(), src_left.begin() + src_consumed);
        src_right.erase(src_right.begin(), src_right.begin() + src_consumed);
        src_pos -= double(src_consumed);
        src_consumed = 0;
      }

      auto append_sample = [&](float l, float r) {
        src_left.push_back(l);
        src_right.push_back(r);
      };

      switch (av_context_->sample_fmt) {
        case AV_SAMPLE_FMT_FLTP: {
          const float* l = reinterpret_cast<const float*>(av_frame_->data[0]);
          const float* r = channels > 1
                              ? reinterpret_cast<const float*>(
                                    av_frame_->data[1])
                              : l;
          for (int i = 0; i < samples; ++i) {
            append_sample(l[i], r[i]);
          }
          break;
        }
        case AV_SAMPLE_FMT_FLT: {
          const float* interleaved =
              reinterpret_cast<const float*>(av_frame_->data[0]);
          for (int i = 0; i < samples; ++i) {
            float l = interleaved[i * channels];
            float r = channels > 1 ? interleaved[i * channels + 1] : l;
            append_sample(l, r);
          }
          break;
        }
        case AV_SAMPLE_FMT_S16P: {
          const int16_t* l =
              reinterpret_cast<const int16_t*>(av_frame_->data[0]);
          const int16_t* r = channels > 1
                                ? reinterpret_cast<const int16_t*>(
                                      av_frame_->data[1])
                                : l;
          for (int i = 0; i < samples; ++i) {
            append_sample(l[i] / 32768.0f, r[i] / 32768.0f);
          }
          break;
        }
        case AV_SAMPLE_FMT_S16: {
          const int16_t* interleaved =
              reinterpret_cast<const int16_t*>(av_frame_->data[0]);
          for (int i = 0; i < samples; ++i) {
            float l = interleaved[i * channels] / 32768.0f;
            float r = channels > 1 ? interleaved[i * channels + 1] / 32768.0f
                                    : l;
            append_sample(l, r);
          }
          break;
        }
        default:
          XELOGE("TitleMusicPlayer: unsupported sample format {}",
                 int(av_context_->sample_fmt));
          break;
      }

      // Resample whatever we now have enough source data for.
      while (has_enough_source(src_pos) && !stop_requested_) {
        size_t i0 = size_t(src_pos);
        double frac = src_pos - double(i0);
        float l = float(src_left[i0] * (1.0 - frac) +
                        src_left[i0 + 1] * frac);
        float r = float(src_right[i0] * (1.0 - frac) +
                        src_right[i0 + 1] * frac);
        out_left.push_back(l);
        out_right.push_back(r);
        src_consumed = i0;
        src_pos += src_step;

        if (out_left.size() >= kChannelSamples) {
          while (paused_ && !stop_requested_) {
            xe::threading::Sleep(std::chrono::milliseconds(20));
          }
          if (stop_requested_) {
            break;
          }
          float* guest_buf =
              memory_->TranslateVirtual<float*>(guest_frame_buffer_);
          float vol = volume_.load(std::memory_order_relaxed);
          for (uint32_t i = 0; i < kChannelSamples; ++i) {
            float l = std::clamp(out_left[i] * vol, -1.0f, 1.0f);
            float r = std::clamp(out_right[i] * vol, -1.0f, 1.0f);
            guest_buf[0 * kChannelSamples + i] = xe::byte_swap(l);
            guest_buf[1 * kChannelSamples + i] = xe::byte_swap(r);
          }
          for (uint32_t ch = 2; ch < kFrameChannels; ++ch) {
            for (uint32_t i = 0; i < kChannelSamples; ++i) {
              guest_buf[ch * kChannelSamples + i] = 0.0f;
            }
          }
          audio_system_->SubmitFrame(client_index_, guest_frame_buffer_);
          out_left.erase(out_left.begin(), out_left.begin() + kChannelSamples);
          out_right.erase(out_right.begin(),
                          out_right.begin() + kChannelSamples);
          xe::threading::Sleep(std::chrono::microseconds(
              kChannelSamples * 1000000 / kFrameRate));
        }
      }
    }
  }

  if (!stop_requested_) {
    // Reached the end of the song on its own, rather than being cut off by
    // Stop() - let the caller (via ConsumeFinishedNaturally, checked from
    // wherever the guest polls XMP status) decide what to play next, the way
    // real hardware would report the player becoming idle.
    finished_naturally_ = true;
  }
}

}  // namespace apu
}  // namespace xe
