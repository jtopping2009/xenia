/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sendspin/noise_session.h"

#include <cstring>

#include "xenia/apu/sendspin/noise_crypto.h"

namespace xe {
namespace apu {
namespace sendspin {

namespace {
constexpr char kProtocolName[] = "Noise_KKpsk2_25519_ChaChaPoly_SHA256";
}  // namespace

NoiseKkPsk2ResponderSession::NoiseKkPsk2ResponderSession(
    const std::array<uint8_t, 32>& local_static_priv,
    const std::array<uint8_t, 32>& local_static_pub,
    const std::array<uint8_t, 32>& remote_static_pub, const uint8_t* prologue,
    size_t prologue_length)
    : s_priv_(local_static_priv),
      s_pub_(local_static_pub),
      rs_pub_(remote_static_pub) {
  // protocol_name is 37 bytes (> 32), so InitializeSymmetric hashes it
  // rather than zero-padding it directly into h.
  h_ = Sha256(reinterpret_cast<const uint8_t*>(kProtocolName),
              sizeof(kProtocolName) - 1);
  ck_ = h_;
  MixHash(prologue, prologue_length);
  // Pre-message patterns, in the fixed canonical order: all "->" tokens
  // (KK's "-> s", the initiator/server's static key) then all "<-" tokens
  // (KK's "<- s", the responder/client's own static key). Only MixHash, no
  // MixKey, per the Noise spec's pre-message handling.
  MixHash(rs_pub_.data(), rs_pub_.size());
  MixHash(s_pub_.data(), s_pub_.size());
}

void NoiseKkPsk2ResponderSession::MixHash(const uint8_t* data,
                                          size_t length) {
  std::vector<uint8_t> buf;
  buf.reserve(h_.size() + length);
  buf.insert(buf.end(), h_.begin(), h_.end());
  buf.insert(buf.end(), data, data + length);
  h_ = Sha256(buf.data(), buf.size());
}

void NoiseKkPsk2ResponderSession::MixKey(
    const std::array<uint8_t, 32>& input_key_material) {
  std::array<uint8_t, 32> new_ck, new_k;
  HkdfSha256(ck_, input_key_material.data(), input_key_material.size(),
             &new_ck, &new_k, nullptr);
  ck_ = new_ck;
  k_ = new_k;
  n_ = 0;
  has_key_ = true;
}

void NoiseKkPsk2ResponderSession::MixKeyAndHash(
    const std::array<uint8_t, 32>& input_key_material) {
  std::array<uint8_t, 32> new_ck, temp_h, new_k;
  HkdfSha256(ck_, input_key_material.data(), input_key_material.size(),
             &new_ck, &temp_h, &new_k);
  ck_ = new_ck;
  MixHash(temp_h.data(), temp_h.size());
  k_ = new_k;
  n_ = 0;
  has_key_ = true;
}

std::vector<uint8_t> NoiseKkPsk2ResponderSession::EncryptAndHash(
    const uint8_t* plaintext, size_t plaintext_length) {
  std::vector<uint8_t> ciphertext;
  if (has_key_) {
    ciphertext = Chacha20Poly1305IetfEncrypt(k_, n_, h_.data(), h_.size(),
                                             plaintext, plaintext_length);
    ++n_;
  } else {
    ciphertext.assign(plaintext, plaintext + plaintext_length);
  }
  MixHash(ciphertext.data(), ciphertext.size());
  return ciphertext;
}

bool NoiseKkPsk2ResponderSession::DecryptAndHash(
    const uint8_t* ciphertext, size_t ciphertext_length,
    std::vector<uint8_t>* out_plaintext) {
  if (has_key_) {
    std::vector<uint8_t> plaintext;
    if (!Chacha20Poly1305IetfDecrypt(k_, n_, h_.data(), h_.size(), ciphertext,
                                     ciphertext_length, &plaintext)) {
      return false;
    }
    ++n_;
    MixHash(ciphertext, ciphertext_length);
    *out_plaintext = std::move(plaintext);
  } else {
    out_plaintext->assign(ciphertext, ciphertext + ciphertext_length);
    MixHash(ciphertext, ciphertext_length);
  }
  return true;
}

bool NoiseKkPsk2ResponderSession::ReadMessage1(
    const uint8_t* message1, size_t message1_length,
    std::vector<uint8_t>* out_payload) {
  if (message1_length < re_pub_.size()) {
    return false;
  }
  std::memcpy(re_pub_.data(), message1, re_pub_.size());
  MixHash(re_pub_.data(), re_pub_.size());
  // KKpsk2 is a PSK handshake, so per the Noise spec (9.2) every "e" token's
  // MixHash is followed by MixKey(e.public_key) too, not just in the
  // message that carries the "psk" token itself.
  MixKey(re_pub_);

  // es: DH(initiator.e, responder.s) - we are the responder, so this is
  // DH(our static private key, their ephemeral public key).
  MixKey(X25519(s_priv_, re_pub_));
  // ss: DH(initiator.s, responder.s) - DH(our static private, their static
  // public).
  MixKey(X25519(s_priv_, rs_pub_));

  const uint8_t* ciphertext = message1 + re_pub_.size();
  size_t ciphertext_length = message1_length - re_pub_.size();
  return DecryptAndHash(ciphertext, ciphertext_length, out_payload);
}

void NoiseKkPsk2ResponderSession::SetPsk(
    const std::array<uint8_t, 32>& psk) {
  pending_psk_ = psk;
}

std::vector<uint8_t> NoiseKkPsk2ResponderSession::WriteMessage2(
    const uint8_t* payload, size_t payload_length) {
  FillRandomBytes(e_priv_.data(), e_priv_.size());
  e_pub_ = X25519PublicKey(e_priv_);
  MixHash(e_pub_.data(), e_pub_.size());
  // See the matching comment in ReadMessage1 - PSK handshakes MixKey every
  // "e" token's public key, not just the one right before "psk".
  MixKey(e_pub_);

  // ee: DH(initiator.e, responder.e) - DH(their ephemeral public, our
  // ephemeral private).
  MixKey(X25519(e_priv_, re_pub_));
  // se: DH(initiator.s, responder.e) - DH(their static public, our
  // ephemeral private).
  MixKey(X25519(e_priv_, rs_pub_));
  // psk (KKpsk2's modifier on message 2): MixKeyAndHash, not a DH.
  MixKeyAndHash(pending_psk_);

  auto ciphertext = EncryptAndHash(payload, payload_length);

  std::vector<uint8_t> out;
  out.reserve(e_pub_.size() + ciphertext.size());
  out.insert(out.end(), e_pub_.begin(), e_pub_.end());
  out.insert(out.end(), ciphertext.begin(), ciphertext.end());
  return out;
}

NoiseKkPsk2ResponderSession::TransportKeys
NoiseKkPsk2ResponderSession::Split() const {
  std::array<uint8_t, 32> c1, c2;
  std::array<uint8_t, 32> zero_ikm{};
  HkdfSha256(ck_, zero_ikm.data(), 0, &c1, &c2, nullptr);
  // By Noise convention the initiator sends with c1 and receives with c2;
  // we are the responder, so it's the other way around.
  TransportKeys keys;
  keys.send_key = c2;
  keys.receive_key = c1;
  return keys;
}

}  // namespace sendspin
}  // namespace apu
}  // namespace xe
