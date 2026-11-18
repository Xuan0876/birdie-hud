// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The panel's link to Birdie (S5): a tiny HTTP + WebSocket server on 127.0.0.1 only, polled from the main loop
// (no threads). WebSocket clients get JSON state messages; plain GET requests are answered with the panel's own
// files, so the same page also opens in a desktop browser (tray: Open panel) when the sim is not running.
#pragma once

#include <winsock2.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace birdie::app {

class PanelServer {
 public:
  struct Events {
    std::function<std::string()> hello;                  // first message to each new WebSocket client
    std::function<void(const std::string&)> message;     // a text message from a client
    std::function<void(const std::string&)> log;
  };

  PanelServer() = default;
  PanelServer(const PanelServer&) = delete;
  PanelServer& operator=(const PanelServer&) = delete;
  ~PanelServer();

  // The page's files by relative name ("birdie.html", "fonts/x.ttf"); nullopt = 404.
  using Files = std::function<std::optional<std::string_view>(const std::string& rel)>;

  // Listens on 127.0.0.1, trying first_port .. first_port + tries - 1.
  bool start(Files files, uint16_t first_port, int tries, std::string* err);
  bool running() const { return listen_ != INVALID_SOCKET; }
  uint16_t port() const { return port_; }
  HANDLE event() const { return event_; }  // signalled on socket activity; poll() resets it
  void poll(const Events& ev);
  void broadcast(const std::string& text);
  int ws_clients() const;

 private:
  struct Client {
    SOCKET s = INVALID_SOCKET;
    bool ws = false;
    bool closing = false;  // close once out is sent
    std::string in, out, frag;
  };
  void accept_all(const Events& ev);
  void on_http(Client& c, const Events& ev);
  void on_ws(Client& c, const Events& ev);
  void serve_file(Client& c, const std::string& path);
  static void flush(Client& c);

  SOCKET listen_ = INVALID_SOCKET;
  HANDLE event_ = nullptr;
  uint16_t port_ = 0;
  bool wsa_ = false;
  Files files_;
  std::vector<Client> clients_;
};

}  // namespace birdie::app
