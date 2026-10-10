/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sendspin/noise_crypto.h"

#include <cstring>
#include <string>

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#pragma comment(lib, "bcrypt.lib")
#else
#include <random>
#endif  // XE_PLATFORM_WIN32

#include "third_party/monocypher/monocypher.c"

// Wrapped in an anonymous namespace so this translation unit gets its own
// internally-linked copy of sha256::SHA256, matching the existing convention
// of including this file directly as source (see xboxkrnl_crypt.cc) without
// risking a duplicate-symbol link error against that other inclusion.
namespace {
#include "third_party/crypto/sha256.cpp"
}  // namespace

namespace xe {
namespace apu {
namespace sendspin {

void FillRandomBytes(uint8_t* out, size_t length) {
#if XE_PLATFORM_WIN32
  BCryptGenRandom(nullptr, out, static_cast<ULONG>(length),
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);
  for (size_t i = 0; i < length; ++i) {
    out[i] = static_cast<uint8_t>(dist(rd));
  }
#endif  // XE_PLATFORM_WIN32
}

std::array<uint8_t, 32> Sha256(const uint8_t* data, size_t length) {
  sha256::SHA256 hasher;
  hasher.add(data, length);
  std::array<uint8_t, 32> out;
  hasher.getHash(out.data());
  return out;
}

std::array<uint8_t, 32> HmacSha256(const uint8_t* key, size_t key_length,
                                    const uint8_t* data, size_t data_length) {
  constexpr size_t kBlockSize = 64;
  uint8_t key_block[kBlockSize] = {0};
  if (key_length > kBlockSize) {
    auto hashed_key = Sha256(key, key_length);
    std::memcpy(key_block, hashed_key.data(), hashed_key.size());
  } else if (key_length > 0) {
    std::memcpy(key_block, key, key_length);
  }

  uint8_t ipad[kBlockSize];
  uint8_t opad[kBlockSize];
  for (size_t i = 0; i < kBlockSize; ++i) {
    ipad[i] = key_block[i] ^ 0x36;
    opad[i] = key_block[i] ^ 0x5C;
  }

  sha256::SHA256 inner;
  inner.add(ipad, kBlockSize);
  inner.add(data, data_length);
  uint8_t inner_hash[32];
  inner.getHash(inner_hash);

  sha256::SHA256 outer;
  outer.add(opad, kBlockSize);
  outer.add(inner_hash, sizeof(inner_hash));
  std::array<uint8_t, 32> out;
  outer.getHash(out.data());
  return out;
}

void HkdfSha256(const std::array<uint8_t, 32>& chaining_key,
                const uint8_t* input_key_material, size_t ikm_length,
                std::array<uint8_t, 32>* out1, std::array<uint8_t, 32>* out2,
                std::array<uint8_t, 32>* out3) {
  auto temp_key =
      HmacSha256(chaining_key.data(), chaining_key.size(), input_key_material,
                 ikm_length);

  uint8_t byte1 = 0x01;
  *out1 = HmacSha256(temp_key.data(), temp_key.size(), &byte1, 1);

  std::vector<uint8_t> buf2(out1->begin(), out1->end());
  buf2.push_back(0x02);
  *out2 = HmacSha256(temp_key.data(), temp_key.size(), buf2.data(),
                      buf2.size());

  if (out3) {
    std::vector<uint8_t> buf3(out2->begin(), out2->end());
    buf3.push_back(0x03);
    *out3 = HmacSha256(temp_key.data(), temp_key.size(), buf3.data(),
                        buf3.size());
  }
}

std::array<uint8_t, 32> X25519PublicKey(
    const std::array<uint8_t, 32>& secret_key) {
  std::array<uint8_t, 32> out;
  crypto_x25519_public_key(out.data(), secret_key.data());
  return out;
}

std::array<uint8_t, 32> X25519(const std::array<uint8_t, 32>& secret_key,
                                const std::array<uint8_t, 32>& public_key) {
  std::array<uint8_t, 32> out;
  crypto_x25519(out.data(), secret_key.data(), public_key.data());
  return out;
}

namespace {

// nonce is 4 zero bytes followed by the little-endian 8-byte Noise counter,
// per the Noise spec's "ChaChaPoly" cipher definition.
std::array<uint8_t, 12> MakeIetfNonce(uint64_t nonce_counter) {
  std::array<uint8_t, 12> nonce{};
  for (int i = 0; i < 8; ++i) {
    nonce[4 + i] = static_cast<uint8_t>(nonce_counter >> (8 * i));
  }
  return nonce;
}

// RFC 8439 pad16: the number of zero bytes needed to pad `length` up to a
// multiple of 16 (0 if already aligned).
size_t Pad16(size_t length) {
  size_t remainder = length % 16;
  return remainder == 0 ? 0 : 16 - remainder;
}

void AppendLe64(std::vector<uint8_t>* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<uint8_t>(value >> (8 * i)));
  }
}

}  // namespace

std::vector<uint8_t> Chacha20Poly1305IetfEncrypt(
    const std::array<uint8_t, 32>& key, uint64_t nonce_counter,
    const uint8_t* ad, size_t ad_length, const uint8_t* plaintext,
    size_t plaintext_length) {
  auto nonce = MakeIetfNonce(nonce_counter);

  uint8_t poly_key[32];
  crypto_chacha20_ietf(poly_key, nullptr, sizeof(poly_key), key.data(),
                       nonce.data(), 0);

  std::vector<uint8_t> ciphertext(plaintext_length);
  if (plaintext_length > 0) {
    crypto_chacha20_ietf(ciphertext.data(), plaintext, plaintext_length,
                         key.data(), nonce.data(), 1);
  }

  std::vector<uint8_t> mac_data;
  mac_data.insert(mac_data.end(), ad, ad + ad_length);
  mac_data.insert(mac_data.end(), Pad16(ad_length), 0);
  mac_data.insert(mac_data.end(), ciphertext.begin(), ciphertext.end());
  mac_data.insert(mac_data.end(), Pad16(ciphertext.size()), 0);
  AppendLe64(&mac_data, ad_length);
  AppendLe64(&mac_data, ciphertext.size());

  uint8_t mac[16];
  crypto_poly1305(mac, mac_data.data(), mac_data.size(), poly_key);
  crypto_wipe(poly_key, sizeof(poly_key));

  std::vector<uint8_t> out;
  out.reserve(ciphertext.size() + sizeof(mac));
  out.insert(out.end(), ciphertext.begin(), ciphertext.end());
  out.insert(out.end(), mac, mac + sizeof(mac));
  return out;
}

bool Chacha20Poly1305IetfDecrypt(const std::array<uint8_t, 32>& key,
                                  uint64_t nonce_counter, const uint8_t* ad,
                                  size_t ad_length, const uint8_t* ciphertext,
                                  size_t ciphertext_length,
                                  std::vector<uint8_t>* out_plaintext) {
  if (ciphertext_length < 16) {
    return false;
  }
  size_t body_length = ciphertext_length - 16;
  const uint8_t* mac_received = ciphertext + body_length;

  auto nonce = MakeIetfNonce(nonce_counter);

  uint8_t poly_key[32];
  crypto_chacha20_ietf(poly_key, nullptr, sizeof(poly_key), key.data(),
                       nonce.data(), 0);

  std::vector<uint8_t> mac_data;
  mac_data.insert(mac_data.end(), ad, ad + ad_length);
  mac_data.insert(mac_data.end(), Pad16(ad_length), 0);
  mac_data.insert(mac_data.end(), ciphertext, ciphertext + body_length);
  mac_data.insert(mac_data.end(), Pad16(body_length), 0);
  AppendLe64(&mac_data, ad_length);
  AppendLe64(&mac_data, body_length);

  uint8_t mac_computed[16];
  crypto_poly1305(mac_computed, mac_data.data(), mac_data.size(), poly_key);
  crypto_wipe(poly_key, sizeof(poly_key));

  if (crypto_verify16(mac_computed, mac_received) != 0) {
    return false;
  }

  out_plaintext->resize(body_length);
  if (body_length > 0) {
    crypto_chacha20_ietf(out_plaintext->data(), ciphertext, body_length,
                         key.data(), nonce.data(), 1);
  }
  return true;
}

namespace {

const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kBase64UrlTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string Base64EncodeGeneric(const uint8_t* data, size_t length,
                                 const char* table, bool pad) {
  std::string out;
  size_t i = 0;
  for (; i + 3 <= length; i += 3) {
    uint32_t chunk = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) |
                     uint32_t(data[i + 2]);
    out.push_back(table[(chunk >> 18) & 0x3F]);
    out.push_back(table[(chunk >> 12) & 0x3F]);
    out.push_back(table[(chunk >> 6) & 0x3F]);
    out.push_back(table[chunk & 0x3F]);
  }
  size_t remaining = length - i;
  if (remaining == 1) {
    uint32_t chunk = uint32_t(data[i]) << 16;
    out.push_back(table[(chunk >> 18) & 0x3F]);
    out.push_back(table[(chunk >> 12) & 0x3F]);
    if (pad) out.append("==");
  } else if (remaining == 2) {
    uint32_t chunk = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out.push_back(table[(chunk >> 18) & 0x3F]);
    out.push_back(table[(chunk >> 12) & 0x3F]);
    out.push_back(table[(chunk >> 6) & 0x3F]);
    if (pad) out.push_back('=');
  }
  return out;
}

bool Base64DecodeGeneric(const std::string& in, bool url_alphabet,
                          std::vector<uint8_t>* out) {
  auto value_of = [url_alphabet](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (url_alphabet) {
      if (c == '-') return 62;
      if (c == '_') return 63;
    } else {
      if (c == '+') return 62;
      if (c == '/') return 63;
    }
    return -1;
  };
  out->clear();
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=') break;
    int v = value_of(c);
    if (v < 0) return false;
    buffer = (buffer << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
    }
  }
  return true;
}

}  // namespace

std::string Base64Encode(const uint8_t* data, size_t length) {
  return Base64EncodeGeneric(data, length, kBase64Table, /*pad=*/true);
}

bool Base64Decode(const std::string& in, std::vector<uint8_t>* out) {
  return Base64DecodeGeneric(in, /*url_alphabet=*/false, out);
}

std::string Base64UrlEncodeNoPadding(const uint8_t* data, size_t length) {
  return Base64EncodeGeneric(data, length, kBase64UrlTable, /*pad=*/false);
}

bool Base64UrlDecodeNoPadding(const std::string& in,
                               std::vector<uint8_t>* out) {
  return Base64DecodeGeneric(in, /*url_alphabet=*/true, out);
}

}  // namespace sendspin
}  // namespace apu
}  // namespace xe
