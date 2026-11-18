// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The small pieces of HTTP/1.1 and WebSocket (RFC 6455) the panel server needs, with no sockets in them so the
// unit tests can check them: request parsing, the handshake key, frame encoding and decoding, allowed origins.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace birdie::app::ws {

struct HttpRequest {
  std::string method, path;
  std::map<std::string, std::string> headers;  // names lower-case, values trimmed
  std::string header(const std::string& name) const;
};

// Parses the request head once "\r\n\r\n" has arrived. Returns the head length (bytes to drop from buf),
// 0 while incomplete, -1 when malformed or too long.
int parse_request(const std::string& buf, HttpRequest& out);

// Sec-WebSocket-Accept for a Sec-WebSocket-Key.
std::string accept_key(const std::string& client_key);

std::string sha1(const std::string& data);  // 20 raw bytes
std::string base64(const std::string& data);

enum class Op : uint8_t { kCont = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

struct Frame {
  Op op = Op::kText;
  bool fin = true;
  std::string payload;  // unmasked
};

// Server -> client frame (never masked).
std::string encode(Op op, const std::string& payload);

// Decodes one client frame from the front of buf. Returns bytes consumed, 0 if more data is needed,
// -1 on a protocol error (unmasked client frame, too large).
int decode(const std::string& buf, Frame& out, size_t max_payload = 1 << 20);

// Pages that may talk to Birdie: the MSFS panel (coui://), a local file, Birdie's own page on loopback, or no
// Origin at all (not a browser). Any other website is refused: browsers do not apply CORS to WebSockets.
bool origin_allowed(const std::string& origin);

}  // namespace birdie::app::ws
