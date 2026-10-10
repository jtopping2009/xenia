/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SENDSPIN_NOISE_CRYPTO_H_
#define XENIA_APU_SENDSPIN_NOISE_CRYPTO_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace apu {
namespace sendspin {

// Primitives needed for Noise_KKpsk2_25519_ChaChaPoly_SHA256, built on top of
// vendored Monocypher (X25519, ChaCha20, Poly1305) and Xenia's existing
// third_party/crypto SHA-256. Kept minimal and scoped to exactly what the
// Noise handshake and transport need - not a general crypto API.

void FillRandomBytes(uint8_t* out, size_t length);

std::array<uint8_t, 32> Sha256(const uint8_t* data, size_t length);

std::array<uint8_t, 32> HmacSha256(const uint8_t* key, size_t key_length,
                                    const uint8_t* data, size_t data_length);

// Noise-spec HKDF: 2 or 3 outputs of 32 bytes each, derived from a 32-byte
// chaining key and input key material of arbitrary length.
void HkdfSha256(const std::array<uint8_t, 32>& chaining_key,
                const uint8_t* input_key_material, size_t ikm_length,
                std::array<uint8_t, 32>* out1, std::array<uint8_t, 32>* out2,
                std::array<uint8_t, 32>* out3);

std::array<uint8_t, 32> X25519PublicKey(
    const std::array<uint8_t, 32>& secret_key);
std::array<uint8_t, 32> X25519(const std::array<uint8_t, 32>& secret_key,
                                const std::array<uint8_t, 32>& public_key);

// RFC 8439 ChaCha20-Poly1305 (IETF, 96-bit nonce = 4 zero bytes followed by
// the little-endian 8-byte Noise nonce counter, per the Noise spec's
// "ChaChaPoly" cipher definition).
std::vector<uint8_t> Chacha20Poly1305IetfEncrypt(
    const std::array<uint8_t, 32>& key, uint64_t nonce_counter,
    const uint8_t* ad, size_t ad_length, const uint8_t* plaintext,
    size_t plaintext_length);

// Returns false (without writing to out_plaintext) if authentication fails.
bool Chacha20Poly1305IetfDecrypt(const std::array<uint8_t, 32>& key,
                                  uint64_t nonce_counter, const uint8_t* ad,
                                  size_t ad_length, const uint8_t* ciphertext,
                                  size_t ciphertext_length,
                                  std::vector<uint8_t>* out_plaintext);

// Plain base64 (RFC 4648, padded) - used only for persisting the local
// identity private key to config, not for any wire format.
std::string Base64Encode(const uint8_t* data, size_t length);
bool Base64Decode(const std::string& in, std::vector<uint8_t>* out);

// base64url, no padding (RFC 4648 section 5) - used for Sendspin client_id /
// server_id and noise/handshake payload data on the wire.
std::string Base64UrlEncodeNoPadding(const uint8_t* data, size_t length);
bool Base64UrlDecodeNoPadding(const std::string& in, std::vector<uint8_t>* out);

}  // namespace sendspin
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SENDSPIN_NOISE_CRYPTO_H_
