/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SENDSPIN_SENDSPIN_PLAYER_SERVICE_H_
#define XENIA_APU_SENDSPIN_SENDSPIN_PLAYER_SERVICE_H_

#include <thread>

namespace xe {
namespace apu {
class AudioSystem;
namespace sendspin {

// Keeps Xenia connected to Music Assistant as a real, always-on Sendspin
// player for the life of the process (milestone 6 of the project plan).
// Runs SendspinClient's session loop (see sendspin_client.h - despite the
// "ConnectAndProbe" name, by milestone 6 a genuinely persistent session) on
// its own background thread, reconnecting with a short backoff whenever a
// session ends for any reason (network drop, server restart, a malformed
// message).
//
// The guide overlay's Music tab doesn't talk to this class directly - it
// just calls Music Assistant's plain HTTP player_queues/play_media API
// (music_assistant_client.h) targeting the player named "Xenia" that this
// service maintains a live connection for. Music Assistant does the rest,
// pushing the stream back over the connection this owns.
class SendspinPlayerService {
 public:
  // Starts the background thread immediately if music_assistant_host is
  // configured (a no-op otherwise - nothing to connect to). `audio_system`
  // is used for the service's entire lifetime and must outlive it; in
  // practice that's the Emulator's own AudioSystem, torn down only at
  // process exit, same as this service.
  explicit SendspinPlayerService(apu::AudioSystem* audio_system);

  // Deliberately does not join the background thread - see the .cc.
  ~SendspinPlayerService();

  SendspinPlayerService(const SendspinPlayerService&) = delete;
  SendspinPlayerService& operator=(const SendspinPlayerService&) = delete;

 private:
  void ThreadMain();

  apu::AudioSystem* audio_system_;
  std::thread thread_;
};

}  // namespace sendspin
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SENDSPIN_SENDSPIN_PLAYER_SERVICE_H_
