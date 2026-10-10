/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SENDSPIN_WEBSOCKET_CLIENT_H_
#define XENIA_APU_SENDSPIN_WEBSOCKET_CLIENT_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xe {
class Socket;
namespace apu {
namespace sendspin {

// A minimal RFC6455 WebSocket client, built directly on xe::Socket, for
// talking to a Sendspin server's plain (non-TLS) ws:// endpoint. Like
// music_assistant::HttpClient, this is not general-purpose: no TLS, no
// compression extensions, one connection per instance. Sendspin's own
// application-layer messages (client/init, the Noise handshake, etc.) all
// ride as WebSocket text/binary frames over this - this class only knows
// about the WebSocket framing, nothing about Sendspin itself.
class WebSocketClient {
 public:
  WebSocketClient();
  ~WebSocketClient();

  // Connects and completes the HTTP Upgrade handshake. Returns false on
  // failure (logged) - a non-101 response, a connection error, etc.
  bool Connect(const std::string& host, uint16_t port,
              const std::string& path);

  bool is_connected() const;

  // Sends a complete text/binary frame (handles client->server masking, as
  // required by RFC6455 - the server's frames back to us are unmasked).
  bool SendText(const std::string& payload);
  bool SendBinary(const std::vector<uint8_t>& payload);

  // Blocks until one complete data frame (text or binary; ping/pong/close
  // are handled internally - a ping is answered with a pong automatically,
  // a close makes this return false) is received or `timeout` elapses.
  // Returns false on timeout, error, or the connection closing.
  bool ReceiveFrame(std::vector<uint8_t>* out_payload, bool* out_is_binary,
                    std::chrono::milliseconds timeout);

  void Close();

 private:
  bool SendFrame(uint8_t opcode, const void* payload, size_t payload_size);
  // Reads and removes exactly one frame from recv_buffer_ if a complete one
  // is present, filling in the out params. Returns false if the buffer
  // doesn't yet contain a complete frame (not an error - caller should keep
  // reading).
  bool TryParseFrame(uint8_t* out_opcode, std::vector<uint8_t>* out_payload);

  std::unique_ptr<Socket> socket_;
  std::vector<uint8_t> recv_buffer_;
};

}  // namespace sendspin
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SENDSPIN_WEBSOCKET_CLIENT_H_
