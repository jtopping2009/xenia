/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_TITLE_MUSIC_PLAYER_H_
#define XENIA_APU_TITLE_MUSIC_PLAYER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace xe {
class Memory;
namespace kernel {
class KernelState;
}  // namespace kernel
namespace apu {

class AudioSystem;

// Decodes and plays a single Xbox 360 title-music (XMP) song file in the
// background, submitting decoded audio directly to the host AudioSystem.
//
// This exists because Xenia's XmpApp (the XAM title-playlist API used by
// games such as Burnout Revenge for their in-game soundtrack) previously only
// tracked playback bookkeeping state without ever actually decoding or
// outputting audio for the referenced song files.
//
// Only WMA-format songs are supported: Xenia's bundled FFmpeg subset only
// compiles the WMA decoder (see third_party/FFmpeg/premake5.lua), not an MP3
// decoder or a demuxer library, so WMA/ASF parsing is implemented by hand
// below rather than via libavformat.
class TitleMusicPlayer {
 public:
  TitleMusicPlayer();
  ~TitleMusicPlayer();

  // Starts playing the song at the given guest virtual file path. Stops any
  // song already playing first. Returns false if the song couldn't be read,
  // parsed, or is in an unsupported format - callers should treat this as a
  // silent no-op rather than a fatal error.
  bool Play(kernel::KernelState* kernel_state, AudioSystem* audio_system,
            const std::u16string& file_path);

  // Stops playback and joins the decode thread. Safe to call even if nothing
  // is playing.
  void Stop();

  void SetPaused(bool paused) { paused_ = paused; }
  void SetVolume(float volume) { volume_ = volume; }

  bool is_playing() const {
    std::lock_guard<std::recursive_mutex> lock(control_mutex_);
    return thread_.joinable();
  }

  // True if the song reached its end on its own (as opposed to being cut off
  // by Stop()) since the last call to this function - callers (checked from
  // whatever guest thread polls status, not from the decode thread) should
  // treat this as "the song finished, decide what to play next" and are
  // expected to follow up with Stop() to join the finished decode thread.
  // Only valid to call once per finish; returns false on subsequent calls
  // until a new song starts.
  bool ConsumeFinishedNaturally() {
    return finished_naturally_.exchange(false);
  }

 private:
  struct AsfStreamInfo {
    uint16_t stream_number = 0;
    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t avg_bytes_per_sec = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    std::vector<uint8_t> extra_data;
  };

  // Parses the ASF container in file_data, filling stream_info_ and
  // payloads_ (the raw compressed audio payloads belonging to the first
  // audio stream found, in playback order). Returns false if the file isn't
  // a parseable single-audio-stream ASF/WMA file.
  bool ParseAsf(const std::vector<uint8_t>& file_data);

  bool OpenDecoder();
  void CloseDecoder();

  // Frees the persistent audio client/guest buffer set up by Play() (see
  // comment there). Only called from the destructor - Stop() intentionally
  // leaves these alone so they can be reused by the next song.
  void ReleasePersistentResources();

  // Runs on decode_thread_. Owns file_data for the lifetime of playback.
  void ThreadMain(std::vector<uint8_t> file_data);

  // Guards Play()/Stop() (and is_playing()) against being run concurrently
  // from different guest kernel threads - e.g. a skip landing on the main
  // dispatch thread at the same moment XMPGetStatus's natural-finish handling
  // runs on whatever thread happens to be polling status. Without this, two
  // overlapping Stop() calls could both see the same non-null
  // guest_frame_buffer_/client_index_ and both free/unregister it, corrupting
  // Xenia's shared guest heap (the same heap XamAlloc uses for the title's
  // own allocations, including its XMP playlist storage). Recursive because
  // Play() calls Stop() internally while already holding the lock.
  mutable std::recursive_mutex control_mutex_;

  Memory* memory_ = nullptr;
  AudioSystem* audio_system_ = nullptr;
  size_t client_index_ = 0;
  bool client_registered_ = false;
  uint32_t guest_frame_buffer_ = 0;

  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> paused_{false};
  std::atomic<bool> finished_naturally_{false};
  std::atomic<float> volume_{1.0f};

  AsfStreamInfo stream_info_;
  std::vector<std::vector<uint8_t>> payloads_;

  AVCodecContext* av_context_ = nullptr;
  AVFrame* av_frame_ = nullptr;
  AVPacket* av_packet_ = nullptr;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_TITLE_MUSIC_PLAYER_H_
