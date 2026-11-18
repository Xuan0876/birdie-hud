// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/ws.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace birdie::app::ws {
namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

}  // namespace

std::string HttpRequest::header(const std::string& name) const {
  auto it = headers.find(lower(name));
  return it == headers.end() ? std::string() : it->second;
}

int parse_request(const std::string& buf, HttpRequest& out) {
  constexpr size_t kMaxHead = 16 * 1024;
  const size_t end = buf.find("\r\n\r\n");
  if (end == std::string::npos) return buf.size() > kMaxHead ? -1 : 0;
  if (end > kMaxHead) return -1;
  out = {};
  size_t pos = 0;
  bool first = true;
  while (pos < end) {
    size_t eol = buf.find("\r\n", pos);
    if (eol == std::string::npos || eol > end) eol = end;
    const std::string line = buf.substr(pos, eol - pos);
    pos = eol + 2;
    if (first) {
      first = false;
      const size_t a = line.find(' '), b = line.rfind(' ');
      if (a == std::string::npos || b == a || !starts_with(line.substr(b + 1), "HTTP/1.")) return -1;
      out.method = line.substr(0, a);
      out.path = line.substr(a + 1, b - a - 1);
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return -1;
    out.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  }
  if (out.method.empty() || out.path.empty()) return -1;
  return static_cast<int>(end + 4);
}

std::string sha1(const std::string& data) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string m = data;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  m.push_back(static_cast<char>(0x80));
  while (m.size() % 64 != 56) m.push_back('\0');
  for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
  for (size_t chunk = 0; chunk < m.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(m.data() + chunk + i * 4);
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  std::string out(20, '\0');
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<char>((h[i] >> (24 - j * 8)) & 0xff);
  return out;
}

std::string base64(const std::string& data) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  for (; i + 2 < data.size(); i += 3) {
    const uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
    out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += t[v & 63];
  }
  if (i + 1 == data.size()) {
    const uint32_t v = p[i] << 16;
    out += t[v >> 18], out += t[(v >> 12) & 63], out += "==";
  } else if (i + 2 == data.size()) {
    const uint32_t v = (p[i] << 16) | (p[i + 1] << 8);
    out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += '=';
  }
  return out;
}

std::string accept_key(const std::string& client_key) {
  return base64(sha1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

std::string encode(Op op, const std::string& payload) {
  std::string f;
  f.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(op)));
  const uint64_t n = payload.size();
  if (n < 126) {
    f.push_back(static_cast<char>(n));
  } else if (n <= 0xffff) {
    f.push_back(126);
    f.push_back(static_cast<char>(n >> 8));
    f.push_back(static_cast<char>(n & 0xff));
  } else {
    f.push_back(127);
    for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((n >> (i * 8)) & 0xff));
  }
  return f + payload;
}

int decode(const std::string& buf, Frame& out, size_t max_payload) {
  const auto* p = reinterpret_cast<const unsigned char*>(buf.data());
  if (buf.size() < 2) return 0;
  const bool masked = p[1] & 0x80;
  if (!masked) return -1;  // clients must mask
  uint64_t n = p[1] & 0x7f;
  size_t pos = 2;
  if (n == 126) {
    if (buf.size() < 4) return 0;
    n = (uint64_t(p[2]) << 8) | p[3];
    pos = 4;
  } else if (n == 127) {
    if (buf.size() < 10) return 0;
    n = 0;
    for (int i = 0; i < 8; ++i) n = (n << 8) | p[2 + i];
    pos = 10;
  }
  if (n > max_payload) return -1;
  if (buf.size() < pos + 4 + n) return 0;
  const unsigned char* mask = p + pos;
  pos += 4;
  out.fin = p[0] & 0x80;
  out.op = static_cast<Op>(p[0] & 0x0f);
  out.payload.assign(buf, pos, static_cast<size_t>(n));
  for (size_t i = 0; i < out.payload.size(); ++i) out.payload[i] = static_cast<char>(out.payload[i] ^ mask[i % 4]);
  return static_cast<int>(pos + n);
}

bool origin_allowed(const std::string& origin_raw) {
  const std::string o = lower(origin_raw);
  if (o.empty() || o == "null") return true;
  if (starts_with(o, "coui://") || starts_with(o, "file://")) return true;
  for (const char* host : {"http://127.0.0.1", "http://localhost"}) {
    const size_t n = std::strlen(host);
    if (o.size() >= n && o.compare(0, n, host) == 0 && (o.size() == n || o[n] == ':' || o[n] == '/')) return true;
  }
  return false;
}

}  // namespace birdie::app::ws
