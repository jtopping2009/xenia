/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sendspin/websocket_client.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <random>

#include "xenia/base/logging.h"
#include "xenia/base/socket.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace sendspin {

namespace {

constexpr std::chrono::milliseconds kHandshakeTimeout(10000);
constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// WebSocket opcodes (RFC6455 section 5.2).
constexpr uint8_t kOpcodeContinuation = 0x0;
constexpr uint8_t kOpcodeText = 0x1;
constexpr uint8_t kOpcodeBinary = 0x2;
constexpr uint8_t kOpcodeClose = 0x8;
constexpr uint8_t kOpcodePing = 0x9;
constexpr uint8_t kOpcodePong = 0xA;

std::string Base64Encode(const uint8_t* data, size_t length) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((length + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= length; i += 3) {
    uint32_t chunk = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) |
                     uint32_t(data[i + 2]);
    out.push_back(kTable[(chunk >> 18) & 0x3F]);
    out.push_back(kTable[(chunk >> 12) & 0x3F]);
    out.push_back(kTable[(chunk >> 6) & 0x3F]);
    out.push_back(kTable[chunk & 0x3F]);
  }
  size_t remaining = length - i;
  if (remaining == 1) {
    uint32_t chunk = uint32_t(data[i]) << 16;
    out.push_back(kTable[(chunk >> 18) & 0x3F]);
    out.push_back(kTable[(chunk >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (remaining == 2) {
    uint32_t chunk = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out.push_back(kTable[(chunk >> 18) & 0x3F]);
    out.push_back(kTable[(chunk >> 12) & 0x3F]);
    out.push_back(kTable[(chunk >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

// Minimal SHA-1 (RFC3174) - only used for the WebSocket handshake's
// Sec-WebSocket-Accept check, which is a protocol-correctness check, not a
// security boundary, so a straightforward implementation is fine here.
std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t length) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476,
           h4 = 0xC3D2E1F0;

  std::vector<uint8_t> msg(data, data + length);
  uint64_t bit_length = uint64_t(length) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) {
    msg.push_back(0);
  }
  for (int i = 7; i >= 0; --i) {
    msg.push_back(uint8_t((bit_length >> (i * 8)) & 0xFF));
  }

  for (size_t chunk_start = 0; chunk_start < msg.size(); chunk_start += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(msg[chunk_start + i * 4]) << 24) |
             (uint32_t(msg[chunk_start + i * 4 + 1]) << 16) |
             (uint32_t(msg[chunk_start + i * 4 + 2]) << 8) |
             uint32_t(msg[chunk_start + i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
      w[i] = (v << 1) | (v >> 31);
    }

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
      e = d;
      d = c;
      c = (b << 30) | (b >> 2);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::array<uint8_t, 20> digest;
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    digest[i * 4 + 0] = uint8_t((hs[i] >> 24) & 0xFF);
    digest[i * 4 + 1] = uint8_t((hs[i] >> 16) & 0xFF);
    digest[i * 4 + 2] = uint8_t((hs[i] >> 8) & 0xFF);
    digest[i * 4 + 3] = uint8_t(hs[i] & 0xFF);
  }
  return digest;
}

std::string GenerateWebSocketKey() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);
  uint8_t key_bytes[16];
  for (auto& b : key_bytes) {
    b = uint8_t(dist(rd));
  }
  return Base64Encode(key_bytes, sizeof(key_bytes));
}

}  // namespace

WebSocketClient::WebSocketClient() = default;
WebSocketClient::~WebSocketClient() { Close(); }

bool WebSocketClient::is_connected() const {
  return socket_ && socket_->is_connected();
}

void WebSocketClient::Close() {
  if (socket_) {
    socket_->Close();
    socket_.reset();
  }
  recv_buffer_.clear();
}

bool WebSocketClient::Connect(const std::string& host, uint16_t port,
                             const std::string& path) {
  socket_ = Socket::Connect(host, port);
  if (!socket_) {
    XELOGE("WebSocketClient: failed to connect to {}:{}", host, port);
    return false;
  }

  std::string key = GenerateWebSocketKey();
  std::string request;
  request += "GET " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: " + key + "\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  request += "\r\n";
  if (!socket_->Send(request)) {
    XELOGE("WebSocketClient: failed to send handshake request");
    Close();
    return false;
  }

  // Read until we have the full HTTP response header block (no body is
  // expected on a 101 Switching Protocols response - any bytes after the
  // header block already belong to the first WebSocket frame and are kept
  // in recv_buffer_ for ReceiveFrame to parse later).
  auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
  std::string header_text;
  size_t header_end = std::string::npos;
  char chunk[4096];
  while (header_end == std::string::npos) {
    if (std::chrono::steady_clock::now() >= deadline) {
      XELOGE("WebSocketClient: timed out waiting for handshake response");
      Close();
      return false;
    }
    xe::threading::Wait(socket_->wait_handle(), false,
                        std::chrono::milliseconds(200));
    size_t received = socket_->Receive(chunk, sizeof(chunk));
    if (received == size_t(-1)) {
      XELOGE("WebSocketClient: connection closed during handshake");
      Close();
      return false;
    }
    if (received > 0) {
      header_text.append(chunk, received);
      header_end = header_text.find("\r\n\r\n");
    }
  }

  std::string extra_bytes = header_text.substr(header_end + 4);
  recv_buffer_.assign(extra_bytes.begin(), extra_bytes.end());
  std::string headers = header_text.substr(0, header_end);

  size_t status_line_end = headers.find("\r\n");
  std::string status_line = headers.substr(0, status_line_end);
  if (status_line.find(" 101 ") == std::string::npos) {
    XELOGE("WebSocketClient: handshake failed, server said: {}", status_line);
    Close();
    return false;
  }

  // Verify Sec-WebSocket-Accept per RFC6455 - defensive, but cheap and
  // catches a misbehaving/incompatible server immediately rather than
  // producing confusing failures later.
  std::string accept_expected =
      Base64Encode(Sha1(reinterpret_cast<const uint8_t*>((key + kWebSocketGuid).data()),
                        key.size() + std::strlen(kWebSocketGuid))
                       .data(),
                   20);
  std::string lower_headers = headers;
  std::transform(lower_headers.begin(), lower_headers.end(),
                 lower_headers.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  size_t accept_pos = lower_headers.find("sec-websocket-accept:");
  bool accept_ok = false;
  if (accept_pos != std::string::npos) {
    size_t value_start = accept_pos + std::strlen("sec-websocket-accept:");
    while (value_start < headers.size() && headers[value_start] == ' ') {
      ++value_start;
    }
    size_t value_end = headers.find("\r\n", value_start);
    std::string accept_actual = headers.substr(value_start, value_end - value_start);
    accept_ok = accept_actual == accept_expected;
  }
  if (!accept_ok) {
    XELOGE(
        "WebSocketClient: Sec-WebSocket-Accept mismatch or missing - "
        "handshake response was not from a real WebSocket server");
    Close();
    return false;
  }

  return true;
}

bool WebSocketClient::SendFrame(uint8_t opcode, const void* payload,
                                size_t payload_size) {
  if (!socket_) {
    return false;
  }
  std::vector<uint8_t> frame;
  frame.push_back(0x80 | opcode);  // FIN=1.

  uint8_t mask_key[4];
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& b : mask_key) {
    b = uint8_t(dist(rd));
  }

  if (payload_size <= 125) {
    frame.push_back(0x80 | uint8_t(payload_size));
  } else if (payload_size <= 0xFFFF) {
    frame.push_back(0x80 | 126);
    frame.push_back(uint8_t((payload_size >> 8) & 0xFF));
    frame.push_back(uint8_t(payload_size & 0xFF));
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; --i) {
      frame.push_back(uint8_t((uint64_t(payload_size) >> (i * 8)) & 0xFF));
    }
  }
  frame.insert(frame.end(), mask_key, mask_key + 4);

  const uint8_t* payload_bytes = reinterpret_cast<const uint8_t*>(payload);
  size_t base = frame.size();
  frame.resize(base + payload_size);
  for (size_t i = 0; i < payload_size; ++i) {
    frame[base + i] = payload_bytes[i] ^ mask_key[i % 4];
  }

  return socket_->Send(frame.data(), frame.size());
}

bool WebSocketClient::SendText(const std::string& payload) {
  return SendFrame(kOpcodeText, payload.data(), payload.size());
}

bool WebSocketClient::SendBinary(const std::vector<uint8_t>& payload) {
  return SendFrame(kOpcodeBinary, payload.data(), payload.size());
}

bool WebSocketClient::TryParseFrame(uint8_t* out_opcode,
                                    std::vector<uint8_t>* out_payload) {
  if (recv_buffer_.size() < 2) {
    return false;
  }
  uint8_t byte0 = recv_buffer_[0];
  uint8_t byte1 = recv_buffer_[1];
  uint8_t opcode = byte0 & 0x0F;
  bool masked = (byte1 & 0x80) != 0;
  uint64_t payload_len = byte1 & 0x7F;
  size_t pos = 2;

  if (payload_len == 126) {
    if (recv_buffer_.size() < pos + 2) return false;
    payload_len =
        (uint64_t(recv_buffer_[pos]) << 8) | uint64_t(recv_buffer_[pos + 1]);
    pos += 2;
  } else if (payload_len == 127) {
    if (recv_buffer_.size() < pos + 8) return false;
    payload_len = 0;
    for (int i = 0; i < 8; ++i) {
      payload_len = (payload_len << 8) | uint64_t(recv_buffer_[pos + i]);
    }
    pos += 8;
  }

  uint8_t mask_key[4] = {0, 0, 0, 0};
  if (masked) {
    if (recv_buffer_.size() < pos + 4) return false;
    std::memcpy(mask_key, recv_buffer_.data() + pos, 4);
    pos += 4;
  }

  if (recv_buffer_.size() < pos + payload_len) {
    return false;
  }

  out_payload->assign(recv_buffer_.begin() + pos,
                      recv_buffer_.begin() + pos + payload_len);
  if (masked) {
    for (size_t i = 0; i < out_payload->size(); ++i) {
      (*out_payload)[i] ^= mask_key[i % 4];
    }
  }
  *out_opcode = opcode;
  recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin() + pos + payload_len);
  return true;
}

bool WebSocketClient::ReceiveFrame(std::vector<uint8_t>* out_payload,
                                   bool* out_is_binary,
                                   std::chrono::milliseconds timeout) {
  if (!socket_) {
    return false;
  }
  auto deadline = std::chrono::steady_clock::now() + timeout;
  char chunk[4096];
  while (true) {
    uint8_t opcode;
    std::vector<uint8_t> payload;
    while (TryParseFrame(&opcode, &payload)) {
      if (opcode == kOpcodeText || opcode == kOpcodeBinary) {
        *out_payload = std::move(payload);
        *out_is_binary = opcode == kOpcodeBinary;
        return true;
      } else if (opcode == kOpcodePing) {
        SendFrame(kOpcodePong, payload.data(), payload.size());
      } else if (opcode == kOpcodeClose) {
        uint16_t close_code = 0;
        std::string reason;
        if (payload.size() >= 2) {
          close_code = (uint16_t(payload[0]) << 8) | uint16_t(payload[1]);
          reason.assign(payload.begin() + 2, payload.end());
        }
        XELOGI("WebSocketClient: server sent Close (code={}, reason='{}')",
               close_code, reason);
        Close();
        return false;
      }
      // Pong and continuation frames: ignored (no fragmented-message
      // reassembly - Sendspin's own JSON/binary messages are small enough
      // not to need it for our purposes).
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    xe::threading::Wait(socket_->wait_handle(), false,
                        std::chrono::milliseconds(200));
    size_t received = socket_->Receive(chunk, sizeof(chunk));
    if (received == size_t(-1)) {
      XELOGI("WebSocketClient: connection closed");
      Close();
      return false;
    }
    if (received > 0) {
      recv_buffer_.insert(recv_buffer_.end(), chunk, chunk + received);
    }
  }
}

}  // namespace sendspin
}  // namespace apu
}  // namespace xe
