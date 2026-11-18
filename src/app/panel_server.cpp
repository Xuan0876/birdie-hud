// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/panel_server.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include "app/ws.h"

#pragma comment(lib, "ws2_32.lib")

namespace birdie::app {
namespace {

constexpr size_t kMaxClients = 8;
constexpr size_t kMaxOut = 4u << 20;  // a client that stops reading is dropped

std::string content_type(const std::filesystem::path& p) {
  const std::string e = p.extension().string();
  if (e == ".html") return "text/html; charset=utf-8";
  if (e == ".css") return "text/css; charset=utf-8";
  if (e == ".js") return "text/javascript; charset=utf-8";
  if (e == ".svg") return "image/svg+xml";
  if (e == ".ttf") return "font/ttf";
  if (e == ".otf") return "font/otf";
  if (e == ".woff2") return "font/woff2";
  if (e == ".png") return "image/png";
  if (e == ".jpg") return "image/jpeg";
  if (e == ".json") return "application/json";
  return "application/octet-stream";
}

std::string status_page(int code, const char* text) {
  std::ostringstream o;
  o << "HTTP/1.1 " << code << ' ' << text << "\r\nContent-Type: text/plain\r\nContent-Length: " << std::strlen(text)
    << "\r\nConnection: close\r\n\r\n"
    << text;
  return o.str();
}

}  // namespace

PanelServer::~PanelServer() {
  for (auto& c : clients_) closesocket(c.s);
  if (listen_ != INVALID_SOCKET) closesocket(listen_);
  if (event_) WSACloseEvent(event_);
  if (wsa_) WSACleanup();
}

bool PanelServer::start(Files files, uint16_t first_port, int tries, std::string* err) {
  files_ = std::move(files);
  WSADATA wd;
  if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
    if (err) *err = "WSAStartup failed";
    return false;
  }
  wsa_ = true;
  event_ = WSACreateEvent();
  for (int i = 0; i < tries; ++i) {
    const uint16_t p = static_cast<uint16_t>(first_port + i);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) break;
    BOOL excl = TRUE;  // nobody else may bind the same port on top of us
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(p);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only: never reachable from the network
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(s, 8) == 0) {
      WSAEventSelect(s, event_, FD_ACCEPT | FD_CLOSE);
      listen_ = s;
      port_ = p;
      return true;
    }
    closesocket(s);
  }
  if (err) *err = "no free port from " + std::to_string(first_port) + " (error " + std::to_string(WSAGetLastError()) + ")";
  return false;
}

int PanelServer::ws_clients() const {
  return static_cast<int>(std::count_if(clients_.begin(), clients_.end(), [](const Client& c) { return c.ws; }));
}

void PanelServer::accept_all(const Events& ev) {
  for (;;) {
    sockaddr_in a = {};
    int n = sizeof(a);
    SOCKET s = accept(listen_, reinterpret_cast<sockaddr*>(&a), &n);
    if (s == INVALID_SOCKET) return;
    if (clients_.size() >= kMaxClients || a.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
      closesocket(s);
      if (ev.log) ev.log("[panel] connection refused (too many clients or not loopback)");
      continue;
    }
    WSAEventSelect(s, event_, FD_READ | FD_WRITE | FD_CLOSE);  // also makes it non-blocking
    Client c;
    c.s = s;
    clients_.push_back(std::move(c));
  }
}

void PanelServer::flush(Client& c) {
  while (!c.out.empty()) {
    const int n = send(c.s, c.out.data(), static_cast<int>(std::min<size_t>(c.out.size(), 1 << 16)), 0);
    if (n <= 0) return;  // WSAEWOULDBLOCK: FD_WRITE signals when there is room again
    c.out.erase(0, static_cast<size_t>(n));
  }
}

void PanelServer::serve_file(Client& c, const std::string& raw_path) {
  std::string path = raw_path.substr(0, raw_path.find_first_of("?#"));
  if (path == "/" || path.empty()) path = "/birdie.html";
  // only plain relative names inside the panel folder
  if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos || path.find(':') != std::string::npos ||
      path.find("//") != std::string::npos) {
    c.out += status_page(403, "Forbidden");
    return;
  }
  const std::string rel = path.substr(1);
  const auto body = files_ ? files_(rel) : std::nullopt;
  if (!body) {
    c.out += status_page(404, "Not Found");
    return;
  }
  std::ostringstream h;
  h << "HTTP/1.1 200 OK\r\nContent-Type: " << content_type(std::filesystem::path(rel)) << "\r\nContent-Length: "
    << body->size() << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  c.out += h.str();
  c.out.append(body->data(), body->size());
}

void PanelServer::on_http(Client& c, const Events& ev) {
  ws::HttpRequest req;
  const int used = ws::parse_request(c.in, req);
  if (used == 0) return;
  c.closing = true;  // unless it becomes a WebSocket below
  if (used < 0) {
    c.out += status_page(400, "Bad Request");
    return;
  }
  c.in.erase(0, static_cast<size_t>(used));
  const bool upgrade = req.header("upgrade").find("websocket") != std::string::npos ||
                       req.header("upgrade").find("WebSocket") != std::string::npos;
  if (!upgrade) {
    if (req.method != "GET") c.out += status_page(405, "Method Not Allowed");
    else serve_file(c, req.path);
    return;
  }
  const std::string key = req.header("sec-websocket-key");
  if (req.method != "GET" || key.empty()) {
    c.out += status_page(400, "Bad Request");
    return;
  }
  if (!ws::origin_allowed(req.header("origin"))) {
    if (ev.log) ev.log("[panel] refused a page from " + req.header("origin"));
    c.out += status_page(403, "Forbidden");
    return;
  }
  c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
           ws::accept_key(key) + "\r\n\r\n";
  c.ws = true;
  c.closing = false;
  if (ev.log) ev.log("[panel] connected (" + (req.header("origin").empty() ? std::string("no origin") : req.header("origin")) + ")");
  if (ev.hello) c.out += ws::encode(ws::Op::kText, ev.hello());
}

void PanelServer::on_ws(Client& c, const Events& ev) {
  for (;;) {
    ws::Frame f;
    const int used = ws::decode(c.in, f, 256 * 1024);
    if (used == 0) return;
    if (used < 0) {
      c.out += ws::encode(ws::Op::kClose, std::string("\x03\xea", 2));  // 1002 protocol error
      c.closing = true;
      c.in.clear();
      return;
    }
    c.in.erase(0, static_cast<size_t>(used));
    switch (f.op) {
      case ws::Op::kText:
      case ws::Op::kCont:
        c.frag += f.payload;
        if (c.frag.size() > 256 * 1024) {
          c.closing = true;
          return;
        }
        if (f.fin) {
          if (ev.message) ev.message(c.frag);
          c.frag.clear();
        }
        break;
      case ws::Op::kPing:
        c.out += ws::encode(ws::Op::kPong, f.payload);
        break;
      case ws::Op::kClose:
        c.out += ws::encode(ws::Op::kClose, f.payload.substr(0, 2));
        c.closing = true;
        return;
      default:
        break;  // binary / pong: ignored
    }
  }
}

void PanelServer::poll(const Events& ev) {
  if (!running()) return;
  WSAResetEvent(event_);  // reset first: anything that happens during the I/O below sets it again
  accept_all(ev);
  for (auto& c : clients_) {
    char buf[8192];
    bool gone = false;
    for (;;) {
      const int n = recv(c.s, buf, sizeof(buf), 0);
      if (n > 0) {
        c.in.append(buf, static_cast<size_t>(n));
        if (c.in.size() > (1u << 20)) gone = true;
        continue;
      }
      if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) gone = true;
      break;
    }
    if (!c.closing && !c.in.empty()) {
      if (!c.ws) on_http(c, ev);
      if (c.ws && !c.closing) on_ws(c, ev);
    }
    flush(c);
    if (gone || (c.closing && c.out.empty()) || c.out.size() > kMaxOut) {
      if (c.ws && ev.log) ev.log("[panel] disconnected");
      if (c.closing && c.out.empty()) shutdown(c.s, SD_SEND);
      closesocket(c.s);
      c.s = INVALID_SOCKET;
    }
  }
  clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [](const Client& c) { return c.s == INVALID_SOCKET; }),
                 clients_.end());
}

void PanelServer::broadcast(const std::string& text) {
  if (!running()) return;
  const std::string f = ws::encode(ws::Op::kText, text);
  for (auto& c : clients_)
    if (c.ws && !c.closing) {
      c.out += f;
      flush(c);
    }
}

}  // namespace birdie::app
