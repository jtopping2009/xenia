/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sendspin/sendspin_player_service.h"

#include <chrono>

#include "xenia/apu/music_assistant/music_assistant_flags.h"
#include "xenia/apu/sendspin/sendspin_client.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace sendspin {

namespace {
// Sendspin's own server listens on 8927 by default (distinct from Music
// Assistant's main HTTP API port) - see connection.md.
constexpr uint16_t kSendspinPort = 8927;
}  // namespace

SendspinPlayerService::SendspinPlayerService(apu::AudioSystem* audio_system)
    : audio_system_(audio_system) {
  if (cvars::music_assistant_host.empty()) {
    return;
  }
  thread_ = std::thread(&SendspinPlayerService::ThreadMain, this);
}

SendspinPlayerService::~SendspinPlayerService() {
  // Deliberately not joined. SendspinClient's session loop has no way to be
  // interrupted from another thread without racing WebSocketClient::Close()
  // against an in-flight blocking socket read on the network thread -
  // Close() resets the underlying Socket, which is not safe to do
  // concurrently with another thread still reading through it - and a
  // healthy session can legitimately run for the rest of the process's
  // lifetime with no natural point to check a stop flag at. Detaching and
  // letting process exit reclaim the thread avoids that race; there's no
  // cross-process state here (like a file write) that an orderly shutdown
  // would need to keep consistent.
  if (thread_.joinable()) {
    thread_.detach();
  }
}

void SendspinPlayerService::ThreadMain() {
  xe::threading::set_name("SendspinPlayer");
  for (;;) {
    SendspinClient client;
    XELOGI("SendspinPlayerService: connecting to {}:{}",
          cvars::music_assistant_host, kSendspinPort);
    bool clean = client.ConnectAndProbe(cvars::music_assistant_host,
                                        kSendspinPort, audio_system_);
    XELOGI(
        "SendspinPlayerService: session ended ({}) - reconnecting shortly",
        clean ? "clean" : "error");
    xe::threading::Sleep(std::chrono::seconds(3));
  }
}

}  // namespace sendspin
}  // namespace apu
}  // namespace xe
