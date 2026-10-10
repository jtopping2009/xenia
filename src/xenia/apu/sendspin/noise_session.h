/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SENDSPIN_NOISE_SESSION_H_
#define XENIA_APU_SENDSPIN_NOISE_SESSION_H_

#include <array>
#include <cstdint>
#include <vector>

namespace xe {
namespace apu {
namespace sendspin {

// Implements exactly the responder side (Xenia is always the Sendspin
// client, and per aiosendspin's driver.py the server is the Noise
// *initiator*) of Noise_KKpsk2_25519_ChaChaPoly_SHA256 - not a general Noise
// implementation. Both static keys are known ahead of time (from the
// cleartext client/init/server/init exchange), so the handshake proper is
// just the two `noise/handshake` messages:
//   message 1 (server -> client): e, es, ss
//   message 2 (client -> server): e, ee, se, psk
class NoiseKkPsk2ResponderSession {
 public:
  NoiseKkPsk2ResponderSession(const std::array<uint8_t, 32>& local_static_priv,
                              const std::array<uint8_t, 32>& local_static_pub,
                              const std::array<uint8_t, 32>& remote_static_pub,
                              const uint8_t* prologue, size_t prologue_length);

  // Processes Noise message 1 (e, es, ss) and decrypts its payload.
  // Returns false if the message is malformed or fails authentication.
  bool ReadMessage1(const uint8_t* message1, size_t message1_length,
                    std::vector<uint8_t>* out_payload);

  // Records the resolved PSK for use by WriteMessage2. The KKpsk2 pattern's
  // "psk" token falls after `se` in message 2's own token sequence (not
  // between message 1 and message 2), so the actual MixKeyAndHash(psk) is
  // deferred and applied at that exact point inside WriteMessage2. Must be
  // called exactly once, before WriteMessage2.
  void SetPsk(const std::array<uint8_t, 32>& psk);

  // Builds Noise message 2 (e, ee, se, psk) with the given plaintext payload.
  std::vector<uint8_t> WriteMessage2(const uint8_t* payload,
                                     size_t payload_length);

  struct TransportKeys {
    std::array<uint8_t, 32> send_key;
    std::array<uint8_t, 32> receive_key;
  };
  // Valid only after WriteMessage2.
  TransportKeys Split() const;

  const std::array<uint8_t, 32>& handshake_hash() const { return h_; }

 private:
  void MixHash(const uint8_t* data, size_t length);
  void MixKey(const std::array<uint8_t, 32>& input_key_material);
  void MixKeyAndHash(const std::array<uint8_t, 32>& input_key_material);
  std::vector<uint8_t> EncryptAndHash(const uint8_t* plaintext,
                                      size_t plaintext_length);
  bool DecryptAndHash(const uint8_t* ciphertext, size_t ciphertext_length,
                      std::vector<uint8_t>* out_plaintext);

  std::array<uint8_t, 32> h_;
  std::array<uint8_t, 32> ck_;
  std::array<uint8_t, 32> k_{};
  uint64_t n_ = 0;
  bool has_key_ = false;
  std::array<uint8_t, 32> pending_psk_{};

  std::array<uint8_t, 32> s_priv_;
  std::array<uint8_t, 32> s_pub_;
  std::array<uint8_t, 32> rs_pub_;
  std::array<uint8_t, 32> e_priv_{};
  std::array<uint8_t, 32> e_pub_{};
  std::array<uint8_t, 32> re_pub_{};
};

}  // namespace sendspin
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SENDSPIN_NOISE_SESSION_H_
