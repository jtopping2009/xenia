/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/music_assistant/http_client.h"

#include <chrono>
#include <cctype>
#include <cstdlib>

#include "xenia/base/logging.h"
#include "xenia/base/socket.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace music_assistant {

namespace {

constexpr std::chrono::milliseconds kOverallTimeout(10000);

std::string BuildRequest(const std::string& method, const std::string& host,
                         const std::string& path,
                         const std::vector<std::pair<std::string, std::string>>&
                             headers,
                         const std::string& body) {
  std::string request;
  request += method;
  request += " ";
  request += path;
  request += " HTTP/1.1\r\n";
  request += "Host: ";
  request += host;
  request += "\r\n";
  request += "Connection: close\r\n";
  for (auto& header : headers) {
    request += header.first;
    request += ": ";
    request += header.second;
    request += "\r\n";
  }
  if (!body.empty()) {
    request += "Content-Length: ";
    request += std::to_string(body.size());
    request += "\r\n";
  }
  request += "\r\n";
  request += body;
  return request;
}

// Reads and parses an HTTP/1.1 response from `socket`, handling
// Content-Length, chunked Transfer-Encoding, and read-until-close bodies.
bool ReadResponse(Socket* socket, HttpResponse* out_response) {
  auto deadline = std::chrono::steady_clock::now() + kOverallTimeout;
  std::string buffer;
  bool headers_parsed = false;
  size_t content_length = std::string::npos;
  bool chunked = false;

  char chunk[4096];
  while (true) {
    if (!headers_parsed) {
      size_t header_end = buffer.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        std::string header_block = buffer.substr(0, header_end);
        size_t line_end = header_block.find("\r\n");
        if (line_end == std::string::npos) line_end = header_block.size();
        std::string status_line = header_block.substr(0, line_end);
        size_t sp1 = status_line.find(' ');
        size_t sp2 =
            sp1 == std::string::npos ? std::string::npos
                                     : status_line.find(' ', sp1 + 1);
        if (sp1 != std::string::npos) {
          out_response->status_code = std::atoi(
              status_line.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
        }
        size_t pos = line_end == header_block.size() ? line_end : line_end + 2;
        while (pos < header_block.size()) {
          size_t next = header_block.find("\r\n", pos);
          if (next == std::string::npos) next = header_block.size();
          std::string line = header_block.substr(pos, next - pos);
          size_t colon = line.find(':');
          if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            for (auto& c : key) c = char(std::tolower(uint8_t(c)));
            if (key == "content-length") {
              content_length = size_t(std::atoll(value.c_str()));
            } else if (key == "transfer-encoding" &&
                      value.find("chunked") != std::string::npos) {
              chunked = true;
            }
          }
          pos = next + 2;
        }
        headers_parsed = true;
        buffer.erase(0, header_end + 4);
      }
    }

    if (headers_parsed) {
      if (chunked) {
        std::string decoded;
        size_t pos = 0;
        bool complete = false;
        bool malformed = false;
        while (true) {
          size_t line_end = buffer.find("\r\n", pos);
          if (line_end == std::string::npos) break;
          std::string size_line = buffer.substr(pos, line_end - pos);
          size_t chunk_size;
          try {
            chunk_size = size_t(std::stoul(size_line, nullptr, 16));
          } catch (...) {
            malformed = true;
            break;
          }
          size_t data_start = line_end + 2;
          if (chunk_size == 0) {
            complete = true;
            break;
          }
          if (buffer.size() < data_start + chunk_size + 2) {
            break;
          }
          decoded.append(buffer, data_start, chunk_size);
          pos = data_start + chunk_size + 2;
        }
        if (malformed) {
          XELOGE("HttpClient: malformed chunked response");
          return false;
        }
        if (complete) {
          out_response->body = std::move(decoded);
          return true;
        }
      } else if (content_length != std::string::npos) {
        if (buffer.size() >= content_length) {
          out_response->body = buffer.substr(0, content_length);
          return true;
        }
      }
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      XELOGE("HttpClient: timed out waiting for response");
      return false;
    }

    xe::threading::Wait(socket->wait_handle(), false,
                        std::chrono::milliseconds(200));
    size_t received = socket->Receive(chunk, sizeof(chunk));
    if (received == size_t(-1)) {
      if (headers_parsed && !chunked && content_length == std::string::npos) {
        out_response->body = buffer;
        return true;
      }
      XELOGE("HttpClient: connection closed before response was complete");
      return false;
    }
    if (received > 0) {
      buffer.append(chunk, received);
    }
  }
}

bool DoRequest(const std::string& method, const std::string& host,
              uint16_t port, const std::string& path,
              const std::vector<std::pair<std::string, std::string>>& headers,
              const std::string& body, HttpResponse* out_response) {
  auto socket = Socket::Connect(host, port);
  if (!socket) {
    XELOGE("HttpClient: failed to connect to {}:{}", host, port);
    return false;
  }
  std::string request = BuildRequest(method, host, path, headers, body);
  if (!socket->Send(request)) {
    XELOGE("HttpClient: failed to send request to {}:{}", host, port);
    return false;
  }
  return ReadResponse(socket.get(), out_response);
}

}  // namespace

bool HttpPost(const std::string& host, uint16_t port, const std::string& path,
              const std::vector<std::pair<std::string, std::string>>& headers,
              const std::string& body, HttpResponse* out_response) {
  return DoRequest("POST", host, port, path, headers, body, out_response);
}

bool HttpGet(const std::string& host, uint16_t port, const std::string& path,
             const std::vector<std::pair<std::string, std::string>>& headers,
             HttpResponse* out_response) {
  return DoRequest("GET", host, port, path, headers, std::string(),
                   out_response);
}

}  // namespace music_assistant
}  // namespace apu
}  // namespace xe
