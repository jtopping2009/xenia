/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xmp_app.h"
#include "xenia/kernel/xthread.h"

#include <cstdlib>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XmpApp::XmpApp(KernelState* kernel_state)
    : App(kernel_state, 0xFA),
      state_(State::kIdle),
      playback_client_(PlaybackClient::kTitle),
      playback_mode_(PlaybackMode::kUnknown),
      repeat_mode_(RepeatMode::kUnknown),
      unknown_flags_(0),
      volume_(1.0f),
      active_playlist_(nullptr),
      active_song_index_(0),
      next_playlist_handle_(1),
      next_song_handle_(1) {}

void XmpApp::OnTitleTerminated() {
  music_player_.Stop();
  auto global_lock = global_critical_region_.Acquire();
  state_ = State::kIdle;
  playback_client_ = PlaybackClient::kSystem;
  active_playlist_ = nullptr;
  active_song_index_ = 0;
  for (auto& it : playlists_) {
    delete it.second;
  }
  playlists_.clear();
  last_seen_storage_values_.clear();
}

X_HRESULT XmpApp::XMPGetStatus(uint32_t state_ptr) {
  if (!XThread::GetCurrentThread()->main_thread()) {
    // Some stupid games will hammer this on a thread - induce a delay
    // here to keep from starving real threads.
    xe::threading::Sleep(std::chrono::milliseconds(1));
  }

  if (music_player_.ConsumeFinishedNaturally()) {
    XELOGI("XMPGetStatus: song finished, going idle");
    music_player_.Stop();
    active_playlist_ = nullptr;
    state_ = State::kIdle;
    OnStateChanged();
  }

  // Diagnostic: watch every playlist's storage_ptr (the guest memory we
  // wrote the real handle into at XMPCreateTitlePlaylist time) for changes
  // coming from anywhere other than us, so we can pin down exactly when and
  // in what game context these get overwritten.
  {
    auto global_lock = global_critical_region_.Acquire();
    for (auto& entry : playlists_) {
      Playlist* pl = entry.second;
      if (!pl->storage_ptr) {
        continue;
      }
      uint32_t current = xe::load_and_swap<uint32_t>(
          memory_->TranslateVirtual(pl->storage_ptr));
      auto seen_it = last_seen_storage_values_.find(pl->storage_ptr);
      if (seen_it == last_seen_storage_values_.end()) {
        last_seen_storage_values_[pl->storage_ptr] = current;
      } else if (seen_it->second != current) {
        XELOGE(
            "XMPGetStatus: storage_ptr={:08X} (originally playlist {:08X}) "
            "changed to {:08X} outside of Xenia's control!",
            pl->storage_ptr, pl->handle, current);
        seen_it->second = current;
      }
    }
  }

  XELOGD("XMPGetStatus({:08X})", state_ptr);
  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(state_ptr),
                               static_cast<uint32_t>(state_));
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPCreateTitlePlaylist(
    uint32_t songs_ptr, uint32_t song_count, uint32_t playlist_name_ptr,
    const std::u16string& playlist_name, uint32_t flags,
    uint32_t out_song_handles, uint32_t out_playlist_handle) {
  XELOGD(
      "XMPCreateTitlePlaylist({:08X}, {:08X}, {:08X}({}), {:08X}, {:08X}, "
      "{:08X})",
      songs_ptr, song_count, playlist_name_ptr, xe::to_utf8(playlist_name),
      flags, out_song_handles, out_playlist_handle);
  auto playlist = std::make_unique<Playlist>();
  playlist->handle = ++next_playlist_handle_;
  playlist->name = playlist_name;
  playlist->flags = flags;
  playlist->storage_ptr = out_playlist_handle;
  if (songs_ptr) {
    for (uint32_t i = 0; i < song_count; ++i) {
      auto song = std::make_unique<Song>();
      song->handle = ++next_song_handle_;
      uint8_t* song_base = memory_->TranslateVirtual(songs_ptr + (i * 36));
      song->file_path =
          xe::load_and_swap<std::u16string>(memory_->TranslateVirtual(
              xe::load_and_swap<uint32_t>(song_base + 0)));
      song->name = xe::load_and_swap<std::u16string>(memory_->TranslateVirtual(
          xe::load_and_swap<uint32_t>(song_base + 4)));
      song->artist =
          xe::load_and_swap<std::u16string>(memory_->TranslateVirtual(
              xe::load_and_swap<uint32_t>(song_base + 8)));
      song->album = xe::load_and_swap<std::u16string>(memory_->TranslateVirtual(
          xe::load_and_swap<uint32_t>(song_base + 12)));
      song->album_artist =
          xe::load_and_swap<std::u16string>(memory_->TranslateVirtual(
              xe::load_and_swap<uint32_t>(song_base + 16)));
      song->genre = xe::load_and_swap<std::u16string>(memory_->TranslateVirtual(
          xe::load_and_swap<uint32_t>(song_base + 20)));
      song->track_number = xe::load_and_swap<uint32_t>(song_base + 24);
      song->duration_ms = xe::load_and_swap<uint32_t>(song_base + 28);
      song->format = static_cast<Song::Format>(
          xe::load_and_swap<uint32_t>(song_base + 32));
      if (out_song_handles) {
        xe::store_and_swap<uint32_t>(
            memory_->TranslateVirtual(out_song_handles + (i * 4)),
            song->handle);
      }
      playlist->songs.emplace_back(std::move(song));
    }
  }
  if (out_playlist_handle) {
    xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_playlist_handle),
                                 playlist->handle);
  }
  // Diagnostic: record every playlist actually created, and the guest
  // address the real handle was written into (out_playlist_handle, aka
  // storage_ptr), so a full-session log can show whether the title ever
  // creates a second/per-race playlist at all, and whether its storage_ptr
  // matches what shows up (or fails to show up) at XMPPlayTitlePlaylist time.
  XELOGI(
      "XMPCreateTitlePlaylist: created playlist handle={:08X} "
      "(written to storage_ptr={:08X}), name='{}', song_count={}",
      playlist->handle, out_playlist_handle, xe::to_utf8(playlist->name),
      song_count);

  auto global_lock = global_critical_region_.Acquire();
  playlists_.insert({playlist->handle, playlist.get()});
  playlist.release();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPDeleteTitlePlaylist(uint32_t playlist_handle) {
  XELOGD("XMPDeleteTitlePlaylist({:08X})", playlist_handle);
  auto global_lock = global_critical_region_.Acquire();
  auto it = playlists_.find(playlist_handle);
  if (it == playlists_.end()) {
    XELOGE("Playlist {:08X} not found", playlist_handle);
    return X_E_NOTFOUND;
  }
  auto playlist = it->second;
  if (playlist == active_playlist_) {
    XMPStop(0);
  }
  playlists_.erase(it);
  delete playlist;
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPlayTitlePlaylist(uint32_t playlist_handle,
                                       uint32_t song_handle) {
  XELOGD("XMPPlayTitlePlaylist({:08X}, {:08X})", playlist_handle, song_handle);
  Playlist* playlist = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = playlists_.find(playlist_handle);
    if (it == playlists_.end()) {
      // Some titles (confirmed via disassembly for Burnout Revenge) request
      // playback through an internal per-track handle table that isn't
      // always correctly populated - this manifests as requests for
      // playlist handles that were never actually created. Rather than
      // going silent (or worse, leaving the title stuck retrying in a tight
      // loop against a permanent failure), fall back to a real, known-good
      // song instead. Pick randomly (excluding whatever's currently active)
      // so that repeated skip requests actually produce variety rather than
      // always landing on the same substitute.
      std::vector<Playlist*> candidates;
      for (auto& entry : playlists_) {
        if (entry.second != active_playlist_) {
          candidates.push_back(entry.second);
        }
      }
      if (candidates.empty() && !playlists_.empty()) {
        // Only one playlist known (and it's the active one) - no real
        // alternative exists, so just keep it going rather than restart it.
        candidates.push_back(active_playlist_);
      }
      if (!candidates.empty()) {
        playlist = candidates[size_t(std::rand()) % candidates.size()];
        XELOGW(
            "XMPPlayTitlePlaylist: playlist {:08X} not found - falling "
            "back to playlist {:08X} instead of failing",
            playlist_handle, playlist->handle);
      } else {
        XELOGE("Playlist {:08X} not found, and no fallback available",
               playlist_handle);
        return X_E_NOTFOUND;
      }
    } else {
      playlist = it->second;
    }
  }

  if (playback_client_ == PlaybackClient::kSystem) {
    // On real hardware this means the dashboard's own custom soundtrack is
    // currently controlling playback and the title should defer to it.
    // Xenia never implements that system-level player, so there is nothing
    // to actually defer to - honoring this would just make title music play
    // never start. Log it and proceed anyway.
    XELOGW(
        "XMPPlayTitlePlaylist: System playback is enabled, but Xenia has no "
        "system player to defer to - playing title music anyway.");
  }

  active_playlist_ = playlist;
  active_song_index_ = 0;
  if (song_handle) {
    for (size_t i = 0; i < playlist->songs.size(); ++i) {
      if (playlist->songs[i]->handle == song_handle) {
        active_song_index_ = int(i);
        break;
      }
    }
  }
  state_ = State::kPlaying;
  OnStateChanged();
  kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 1);

  if (active_playlist_->songs.empty()) {
    return X_E_SUCCESS;
  }
  Song* song = active_playlist_->songs[active_song_index_].get();
  if (song->format != Song::Format::kWma) {
    XELOGW(
        "XMPPlayTitlePlaylist: song '{}' is not WMA - Xenia's bundled "
        "FFmpeg doesn't include an MP3 decoder, so it can't be played.",
        xe::to_utf8(song->name));
    return X_E_SUCCESS;
  }
  if (music_player_.Play(kernel_state_,
                         kernel_state_->emulator()->audio_system(),
                         song->file_path)) {
    XELOGI("XMPPlayTitlePlaylist: now playing '{}' (playlist {:08X})",
           xe::to_utf8(song->name), playlist_handle);
  } else {
    XELOGW("XMPPlayTitlePlaylist: failed to start playback of '{}'",
           xe::to_utf8(song->name));
  }
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPContinue() {
  XELOGD("XMPContinue()");
  if (state_ == State::kPaused) {
    state_ = State::kPlaying;
  }
  music_player_.SetPaused(false);
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPStop(uint32_t unk) {
  assert_zero(unk);
  XELOGD("XMPStop({:08X})", unk);
  active_playlist_ = nullptr;  // ?
  active_song_index_ = 0;
  state_ = State::kIdle;
  music_player_.Stop();
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPause() {
  XELOGD("XMPPause()");
  if (state_ == State::kPlaying) {
    state_ = State::kPaused;
  }
  music_player_.SetPaused(true);
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPNext() {
  XELOGD("XMPNext()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  active_song_index_ =
      (active_song_index_ + 1) % active_playlist_->songs.size();
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPrevious() {
  XELOGD("XMPPrevious()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  if (!active_song_index_) {
    active_song_index_ = static_cast<int>(active_playlist_->songs.size()) - 1;
  } else {
    --active_song_index_;
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

void XmpApp::OnStateChanged() {
  kernel_state_->BroadcastNotification(kMsgStateChanged,
                                       static_cast<uint32_t>(state_));
}

X_HRESULT XmpApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00070002: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client = xe::load_and_swap<uint32_t>(buffer + 0);
      uint32_t storage_ptr = xe::load_and_swap<uint32_t>(buffer + 4);
      uint32_t song_handle = xe::load_and_swap<uint32_t>(buffer + 8);  // 0?
      uint32_t playlist_handle =
          xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(storage_ptr));
      assert_true(xmp_client == 0x00000002);
      // Diagnostic: the title hands us storage_ptr (a guest address it
      // allocated for XMPCreateTitlePlaylist to write the real handle into),
      // and we dereference it above to get playlist_handle. If storage_ptr
      // itself looks like a plausible heap address (Xbox 360 title heaps are
      // typically 0x40000000-0x7FFFFFFF or so) but playlist_handle doesn't
      // match anything we created, that memory was never written by a real
      // Create call. If storage_ptr itself is suspiciously small (looks like
      // a raw index rather than an address), the title isn't going through
      // storage_ptr indirection at all for this call site.
      XELOGD(
          "XMPPlayTitlePlaylist dispatch: raw storage_ptr={:08X} -> "
          "dereferenced playlist_handle={:08X}, song_handle={:08X}",
          storage_ptr, playlist_handle, song_handle);
      return XMPPlayTitlePlaylist(playlist_handle, song_handle);
    }
    case 0x00070003: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = xe::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPContinue();
    }
    case 0x00070004: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = xe::load_and_swap<uint32_t>(buffer + 0);
      uint32_t unk = xe::load_and_swap<uint32_t>(buffer + 4);
      assert_true(xmp_client == 0x00000002);
      return XMPStop(unk);
    }
    case 0x00070005: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = xe::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPause();
    }
    case 0x00070006: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = xe::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPNext();
    }
    case 0x00070007: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = xe::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPrevious();
    }
    case 0x00070008: {
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> playback_mode;
        xe::be<uint32_t> repeat_mode;
        xe::be<uint32_t> flags;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 ||
                  args->xmp_client == 0x00000000);
      XELOGD("XMPSetPlaybackBehavior({:08X}, {:08X}, {:08X})",
             uint32_t(args->playback_mode), uint32_t(args->repeat_mode),
             uint32_t(args->flags));
      playback_mode_ = static_cast<PlaybackMode>(uint32_t(args->playback_mode));
      repeat_mode_ = static_cast<RepeatMode>(uint32_t(args->repeat_mode));
      unknown_flags_ = args->flags;
      kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 0);
      return X_E_SUCCESS;
    }
    case 0x00070009: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = xe::load_and_swap<uint32_t>(buffer + 0);
      uint32_t state_ptr =
          xe::load_and_swap<uint32_t>(buffer + 4);  // out ptr to 4b - expect 0
      assert_true(xmp_client == 0x00000002);
      return XMPGetStatus(state_ptr);
    }
    case 0x0007000B: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> volume_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      XELOGD("XMPGetVolume({:08X})", uint32_t(args->volume_ptr));
      xe::store_and_swap<float>(memory_->TranslateVirtual(args->volume_ptr),
                                volume_);
      return X_E_SUCCESS;
    }
    case 0x0007000C: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<float> value;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      XELOGD("XMPSetVolume({:g})", float(args->value));
      volume_ = args->value;
      music_player_.SetVolume(volume_);
      return X_E_SUCCESS;
    }
    case 0x0007000D: {
      assert_true(!buffer_length || buffer_length == 36);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> storage_ptr;
        xe::be<uint32_t> storage_size;
        xe::be<uint32_t> songs_ptr;
        xe::be<uint32_t> song_count;
        xe::be<uint32_t> playlist_name_ptr;
        xe::be<uint32_t> flags;
        xe::be<uint32_t> song_handles_ptr;
        xe::be<uint32_t> playlist_handle_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 36);

      xe::store_and_swap<uint32_t>(
          memory_->TranslateVirtual(args->playlist_handle_ptr),
          args->storage_ptr);
      assert_true(args->xmp_client == 0x00000002 ||
                  args->xmp_client == 0x00000000);
      std::u16string playlist_name;
      if (!args->playlist_name_ptr) {
        playlist_name = u"";
      } else {
        playlist_name = xe::load_and_swap<std::u16string>(
            memory_->TranslateVirtual(args->playlist_name_ptr));
      }
      // dummy_alloc_ptr is the result of a XamAlloc of storage_size.
      assert_true(uint32_t(args->storage_size) ==
                  4 + uint32_t(args->song_count) * 128);
      return XMPCreateTitlePlaylist(args->songs_ptr, args->song_count,
                                    args->playlist_name_ptr, playlist_name,
                                    args->flags, args->song_handles_ptr,
                                    args->storage_ptr);
    }
    case 0x0007000E: {
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> unk_ptr;  // 0
        xe::be<uint32_t> info_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      auto info = memory_->TranslateVirtual(args->info_ptr);
      assert_true(args->xmp_client == 0x00000002);
      assert_zero(args->unk_ptr);
      XELOGE("XMPGetInfo?({:08X}, {:08X})", uint32_t(args->unk_ptr),
             uint32_t(args->info_ptr));
      if (!active_playlist_) {
        return X_E_FAIL;
      }
      auto& song = active_playlist_->songs[active_song_index_];
      xe::store_and_swap<uint32_t>(info + 0, song->handle);
      xe::store_and_swap<std::u16string>(info + 4 + 572 + 0, song->name);
      xe::store_and_swap<std::u16string>(info + 4 + 572 + 40, song->artist);
      xe::store_and_swap<std::u16string>(info + 4 + 572 + 80, song->album);
      xe::store_and_swap<std::u16string>(info + 4 + 572 + 120,
                                         song->album_artist);
      xe::store_and_swap<std::u16string>(info + 4 + 572 + 160, song->genre);
      xe::store_and_swap<uint32_t>(info + 4 + 572 + 200, song->track_number);
      xe::store_and_swap<uint32_t>(info + 4 + 572 + 204, song->duration_ms);
      xe::store_and_swap<uint32_t>(info + 4 + 572 + 208,
                                   static_cast<uint32_t>(song->format));
      return X_E_SUCCESS;
    }
    case 0x00070013: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> storage_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      uint32_t playlist_handle = xe::load_and_swap<uint32_t>(
          memory_->TranslateVirtual(args->storage_ptr));
      assert_true(args->xmp_client == 0x00000002 ||
                  args->xmp_client == 0x00000000);
      return XMPDeleteTitlePlaylist(playlist_handle);
    }
    case 0x0007001A: {
      // XMPSetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> controller;
        xe::be<uint32_t> playback_client;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(
          (args->xmp_client == 0x00000002 && args->controller == 0x00000000) ||
          (args->xmp_client == 0x00000000 && args->controller == 0x00000001));
      XELOGD("XMPSetPlaybackController({:08X}, {:08X})",
             uint32_t(args->controller), uint32_t(args->playback_client));

      playback_client_ = PlaybackClient(uint32_t(args->playback_client));
      kernel_state_->BroadcastNotification(kMsgPlaybackControllerChanged,
                                           !args->playback_client);
      return X_E_SUCCESS;
    }
    case 0x0007001B: {
      // XMPGetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> controller_ptr;
        xe::be<uint32_t> locked_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002);
      XELOGD("XMPGetPlaybackController({:08X}, {:08X}, {:08X})",
             uint32_t(args->xmp_client), uint32_t(args->controller_ptr),
             uint32_t(args->locked_ptr));
      xe::store_and_swap<uint32_t>(
          memory_->TranslateVirtual(args->controller_ptr), 0);
      xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->locked_ptr),
                                   0);

      if (!XThread::GetCurrentThread()->main_thread()) {
        // Atrain spawns a thread 82437FD0 to call this in a tight loop forever.
        xe::threading::Sleep(std::chrono::milliseconds(10));
      }

      return X_E_SUCCESS;
    }
    case 0x00070029: {
      // XMPGetPlaybackBehavior
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> playback_mode_ptr;
        xe::be<uint32_t> repeat_mode_ptr;
        xe::be<uint32_t> unk3_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 ||
                  args->xmp_client == 0x00000000);
      XELOGD("XMPGetPlaybackBehavior({:08X}, {:08X}, {:08X})",
             uint32_t(args->playback_mode_ptr), uint32_t(args->repeat_mode_ptr),
             uint32_t(args->unk3_ptr));
      if (args->playback_mode_ptr) {
        xe::store_and_swap<uint32_t>(
            memory_->TranslateVirtual(args->playback_mode_ptr),
            static_cast<uint32_t>(playback_mode_));
      }
      if (args->repeat_mode_ptr) {
        xe::store_and_swap<uint32_t>(
            memory_->TranslateVirtual(args->repeat_mode_ptr),
            static_cast<uint32_t>(repeat_mode_));
      }
      if (args->unk3_ptr) {
        xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->unk3_ptr),
                                     unknown_flags_);
      }
      return X_E_SUCCESS;
    }
    case 0x0007002E: {
      assert_true(!buffer_length || buffer_length == 12);
      // Query of size for XamAlloc - the result of the alloc is passed to
      // 0x0007000D.
      struct {
        xe::be<uint32_t> xmp_client;
        xe::be<uint32_t> song_count;
        xe::be<uint32_t> size_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002 ||
                  args->xmp_client == 0x00000000);
      // We don't use the storage, so just fudge the number.
      xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->size_ptr),
                                   4 + uint32_t(args->song_count) * 128);
      return X_E_SUCCESS;
    }
    case 0x0007003D: {
      // XMPCaptureOutput - not sure how this works :/
      XELOGD("XMPCaptureOutput(...)");
      assert_always("XMP output not unimplemented");
      return X_E_FAIL;
    }
  }
  XELOGE(
      "Unimplemented XMP message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
