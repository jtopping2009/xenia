/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_MUSIC_ASSISTANT_HTTP_CLIENT_H_
#define XENIA_APU_MUSIC_ASSISTANT_HTTP_CLIENT_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xe {
namespace apu {
namespace music_assistant {

struct HttpResponse {
  int status_code = 0;
  std::string body;
};

// Minimal synchronous HTTP/1.1 client built directly on xe::Socket, for
// talking to a Music Assistant server's plain (non-TLS) local HTTP API.
// Not a general-purpose HTTP client: no TLS, no redirects, no connection
// reuse - each call opens a fresh connection and closes it. That's fine for
// occasional one-shot JSON RPC calls, which is all this is used for.
//
// Supports Content-Length and chunked Transfer-Encoding response bodies
// (aiohttp, which Music Assistant's server is built on, may use either
// depending on response size), falling back to read-until-close otherwise.
bool HttpPost(const std::string& host, uint16_t port, const std::string& path,
              const std::vector<std::pair<std::string, std::string>>& headers,
              const std::string& body, HttpResponse* out_response);

bool HttpGet(const std::string& host, uint16_t port, const std::string& path,
             const std::vector<std::pair<std::string, std::string>>& headers,
             HttpResponse* out_response);

}  // namespace music_assistant
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_MUSIC_ASSISTANT_HTTP_CLIENT_H_
