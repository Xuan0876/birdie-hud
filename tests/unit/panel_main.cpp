// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Unit tests for the panel link (S5): HTTP request head, WebSocket handshake and frames, allowed origins, and the
// settings the panel may change.
#include <cmath>
#include <cstdio>
#include <string>

#include "app/hotkeys.h"
#include "app/panel_proto.h"
#include "app/ws.h"

namespace {

int g_fail = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      ++g_fail;                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

namespace ws = birdie::app::ws;

std::string hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) o += d[c >> 4], o += d[c & 15];
  return o;
}

void test_sha1_base64() {
  CHECK(hex(ws::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(hex(ws::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  // two blocks
  CHECK(hex(ws::sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(ws::base64("") == "" && ws::base64("f") == "Zg==" && ws::base64("fo") == "Zm8=" && ws::base64("foo") == "Zm9v");
  CHECK(ws::base64("foobar") == "Zm9vYmFy");
  // RFC 6455 section 1.3
  CHECK(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

void test_request() {
  ws::HttpRequest r;
  CHECK(ws::parse_request("GET / HTTP/1.1\r\nHost: x", r) == 0);  // incomplete
  const std::string head =
      "GET /ws?x=1 HTTP/1.1\r\nHost: 127.0.0.1:47820\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key:  dGhlIHNhbXBsZSBub25jZQ== \r\nOrigin: coui://html_ui\r\n\r\n";
  CHECK(ws::parse_request(head + "extra", r) == static_cast<int>(head.size()));
  CHECK(r.method == "GET" && r.path == "/ws?x=1");
  CHECK(r.header("sec-websocket-key") == "dGhlIHNhbXBsZSBub25jZQ==" && r.header("ORIGIN") == "coui://html_ui");
  CHECK(ws::parse_request("nonsense\r\n\r\n", r) == -1);
  CHECK(ws::parse_request("GET / HTTP/1.1\r\nno colon\r\n\r\n", r) == -1);
  CHECK(ws::parse_request(std::string(20000, 'a'), r) == -1);
}

std::string masked(uint8_t first, const std::string& payload) {
  const unsigned char key[4] = {0x37, 0xfa, 0x21, 0x3d};
  std::string f;
  f.push_back(static_cast<char>(first));
  if (payload.size() < 126) {
    f.push_back(static_cast<char>(0x80 | payload.size()));
  } else {
    f.push_back(static_cast<char>(0x80 | 126));
    f.push_back(static_cast<char>(payload.size() >> 8));
    f.push_back(static_cast<char>(payload.size() & 0xff));
  }
  f.append(reinterpret_cast<const char*>(key), 4);
  for (size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
  return f;
}

void test_frames() {
  // RFC 6455 section 5.7: masked "Hello"
  const std::string hello("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11);
  ws::Frame f;
  CHECK(ws::decode(hello.substr(0, 6), f) == 0);
  CHECK(ws::decode(hello, f) == 11 && f.fin && f.op == ws::Op::kText && f.payload == "Hello");
  CHECK(ws::encode(ws::Op::kText, "Hello") == std::string("\x81\x05Hello", 7));
  const std::string big(300, 'x');
  const std::string e = ws::encode(ws::Op::kText, big);
  CHECK(e.size() == 304 && static_cast<unsigned char>(e[1]) == 126 && e.substr(4) == big);
  CHECK(ws::encode(ws::Op::kText, std::string(70000, 'y')).size() == 70010);
  const std::string m = masked(0x81, big);
  CHECK(ws::decode(m, f) == static_cast<int>(m.size()) && f.payload == big);
  CHECK(ws::decode(std::string("\x81\x05Hello", 7), f) == -1);    // unmasked client frame
  CHECK(ws::decode(masked(0x81, big), f, 100) == -1);              // too large
  CHECK(ws::decode(masked(0x88, "\x03\xe8"), f) == 8 && f.op == ws::Op::kClose);
  CHECK(ws::decode(masked(0x01, "par"), f) > 0 && !f.fin);         // first fragment
}

void test_origins() {
  CHECK(ws::origin_allowed("") && ws::origin_allowed("null"));
  CHECK(ws::origin_allowed("coui://html_ui") && ws::origin_allowed("file://"));
  CHECK(ws::origin_allowed("http://127.0.0.1:47820") && ws::origin_allowed("http://localhost"));
  CHECK(ws::origin_allowed("HTTP://LOCALHOST:5173"));
  CHECK(!ws::origin_allowed("https://example.com") && !ws::origin_allowed("http://localhost.evil.com"));
  CHECK(!ws::origin_allowed("http://127.0.0.1.evil.com") && !ws::origin_allowed("http://192.0.2.1"));
}

void test_settings() {
  birdie::Settings s;
  using birdie::app::apply_panel_set;
  CHECK(apply_panel_set(s, "opacity", 0.55) && std::abs(s.opacity - 0.55) < 1e-9);
  CHECK(apply_panel_set(s, "opacity", 0.0) && std::abs(s.opacity - 0.15) < 1e-9);  // never invisible
  CHECK(apply_panel_set(s, "hud_scale", 0.75) && s.hud_scale == 0.75);
  CHECK(apply_panel_set(s, "hud_scale", 9) && s.hud_scale == 1.5);
  CHECK(apply_panel_set(s, "glide_deg", 3.14159) && std::abs(s.glide_deg - 3.1) < 1e-9);
  CHECK(apply_panel_set(s, "glide_deg", 3.3) && birdie::settings_to_json(s)["glide_deg"].dump() == "3.3");
  CHECK(!apply_panel_set(s, "glide_deg", "3.5") && std::abs(s.glide_deg - 3.3) < 1e-9);  // wrong type: unchanged
  CHECK(apply_panel_set(s, "show_runway", false) && !s.show_runway);
  CHECK(!apply_panel_set(s, "show_runway", 1) && !s.show_runway);
  CHECK(apply_panel_set(s, "visible", false) && !s.visible);
  CHECK(apply_panel_set(s, "lead_ms", 40.4) && s.lead_ms == 40);
  CHECK(apply_panel_set(s, "lead_ms", 1000) && s.lead_ms == 300);
  CHECK(apply_panel_set(s, "ladder_step_deg", 0) && s.ladder_step_deg == 0);
  CHECK(apply_panel_set(s, "flare_tol_deg", 0.5) && s.flare_tol_deg == 0.5);
  CHECK(apply_panel_set(s, "language", "en") && s.language == "en");
  CHECK(!apply_panel_set(s, "language", "fr") && s.language == "en");
  CHECK(!apply_panel_set(s, "box_w", 1.0) && !apply_panel_set(s, "ra_by_aircraft", nlohmann::json::object()));
  CHECK(apply_panel_set(s, "bird_ra_below_ft", 20) && s.bird_ra_below_ft == 50);
  CHECK(apply_panel_set(s, "bird_ra_below_ft", 1000) && s.bird_ra_below_ft == 1000);
  CHECK(s.ui_style == "tool" && apply_panel_set(s, "ui_style", "dusk") && s.ui_style == "dusk");
  CHECK(!apply_panel_set(s, "ui_style", "neon") && !apply_panel_set(s, "ui_style", 1) && s.ui_style == "dusk");
  CHECK(apply_panel_set(s, "auto_hide", true) && s.auto_hide);
  CHECK(apply_panel_set(s, "auto_show_kt", 60.4) && s.auto_show_kt == 60);
  CHECK(apply_panel_set(s, "auto_hide_ft", 2500) && s.auto_hide_ft == 2500);
  CHECK(apply_panel_set(s, "auto_hide_s", -5) && s.auto_hide_s == 0);
  CHECK(birdie::settings_from_json(birdie::settings_to_json(s)).ui_style == "dusk");
  // the camera source (core/camera.h): two names only
  {
    birdie::Settings c;
    CHECK(c.camera_source == "chaseplane" && apply_panel_set(c, "camera_source", "msfs") && c.camera_source == "msfs");
    CHECK(!apply_panel_set(c, "camera_source", "webcam") && !apply_panel_set(c, "camera_source", 1) && c.camera_source == "msfs");
    CHECK(birdie::settings_from_json(birdie::settings_to_json(c)).camera_source == "msfs");
  }
  // terrain: off by default, strings and the resolution only from their lists, numbers clamped
  CHECK(!s.terrain_on && s.terrain_look == "fill" && s.terrain_res == 30 && s.terrain_cache_gb == 2.0);
  CHECK(apply_panel_set(s, "terrain_on", true) && s.terrain_on);
  CHECK(apply_panel_set(s, "terrain_look", "ridges") && s.terrain_look == "ridges");
  CHECK(!apply_panel_set(s, "terrain_look", "mesh") && s.terrain_look == "ridges");
  CHECK(apply_panel_set(s, "terrain_res", 90) && s.terrain_res == 90 && !apply_panel_set(s, "terrain_res", 60));
  CHECK(apply_panel_set(s, "terrain_cache_gb", 0.1) && s.terrain_cache_gb == 0.5);
  CHECK(apply_panel_set(s, "terrain_strength", 1.234) && s.terrain_strength == 1.25);
  CHECK(!apply_panel_set(s, "terrain_dir", "D:/x"));  // the folder changes only through the move action
  {
    const auto r = birdie::settings_from_json(birdie::settings_to_json(s));
    CHECK(r.terrain_on && r.terrain_look == "ridges" && r.terrain_res == 90 && r.terrain_cache_gb == 0.5);
    nlohmann::json bad = {{"terrain_look", "x"}, {"terrain_res", 45}, {"terrain_cache_gb", 1e9}, {"terrain_strength", -1}};
    const auto b = birdie::settings_from_json(bad);
    CHECK(b.terrain_look == "fill" && b.terrain_res == 30 && b.terrain_cache_gb == 64 && b.terrain_strength == 0.2);
  }
  const auto j = birdie::app::panel_settings(s);
  CHECK(j["terrain_look"] == "ridges" && j["terrain_res"] == 90 && j["terrain_on"] == true && !j.contains("terrain_dir"));
  CHECK(j.size() == 87 && j["camera_source"] == "chaseplane" && j["hotkeys"]["toggle"] == "Ctrl+Alt+Insert" && j["sty_fpv"] == "airbus" && j["tdz_rule"] == "icao" && j["show_tdz"] == true && j["ui_style"] == "dusk" && j["auto_hide"] == true && j["auto_show_kt"] == 60 && j["xwind_mode"] == false && j["show_bird_ra"] == true && j["bird_ra_below_ft"] == 1000 && j["show_runway"] == false && j["lead_ms"] == 300 && j["language"] == "en");
  CHECK(!j.contains("box_w") && !j.contains("ra_by_aircraft"));
}

void test_hotkeys() {
  using birdie::app::canonical_combo;
  using birdie::app::parse_combo;
  CHECK(canonical_combo("ctrl+alt+insert") == "Ctrl+Alt+Insert");
  CHECK(canonical_combo("Shift + Ctrl + pageup") == "Ctrl+Shift+PgUp");
  CHECK(canonical_combo("Ctrl+Num+") == "Ctrl+Num+" && canonical_combo("alt+f") == "Alt+F");
  CHECK(canonical_combo("F9") == "F9" && canonical_combo("Q").empty() && canonical_combo("Ctrl+Bogus").empty());
  CHECK(canonical_combo("Hyper+A").empty() && canonical_combo("").empty() && canonical_combo("Ctrl+").empty());
  const auto c = parse_combo("Ctrl+Alt+Shift+End");
  CHECK(c && c->vk == 0x23 && c->mod == (0x0002u | 0x0001u | 0x0004u));
  // every action has a default, and the defaults parse
  const auto& d = birdie::default_hotkeys();
  CHECK(d.size() == birdie::app::hotkey_actions().size());
  for (const auto& a : birdie::app::hotkey_actions()) {
    const auto it = d.find(a.name);
    CHECK(it != d.end() && (it->second.empty() || canonical_combo(it->second) == it->second));
  }
  birdie::Settings s;
  using birdie::app::apply_panel_set;
  CHECK(apply_panel_set(s, "hotkey.panel", "ctrl+alt+p") && s.hotkeys["panel"] == "Ctrl+Alt+P");
  CHECK(apply_panel_set(s, "hotkey.anim_test", "Ctrl+Alt+Insert") && s.hotkeys["anim_test"] == "Ctrl+Alt+Insert" &&
        s.hotkeys["toggle"].empty());  // taken over
  CHECK(!apply_panel_set(s, "hotkey.panel", "P") && s.hotkeys["panel"] == "Ctrl+Alt+P");
  CHECK(!apply_panel_set(s, "hotkey.nope", "Ctrl+A") && !apply_panel_set(s, "hotkey.panel", 3));
  CHECK(apply_panel_set(s, "hotkey.quit", "") && s.hotkeys["quit"].empty());
  const auto r = birdie::settings_from_json(birdie::settings_to_json(s));
  CHECK(r.hotkeys.at("panel") == "Ctrl+Alt+P" && r.hotkeys.at("quit").empty() && r.hotkeys.size() == d.size());
  nlohmann::json bad = {{"hotkeys", {{"panel", 7}, {"weird", "Ctrl+A"}}}};
  const auto b = birdie::settings_from_json(bad);
  CHECK(b.hotkeys == d);
}

}  // namespace

int main() {
  test_sha1_base64();
  test_request();
  test_frames();
  test_origins();
  test_settings();
  test_hotkeys();
  std::printf(g_fail ? "panel tests FAILED (%d)\n" : "panel tests PASSED\n", g_fail);
  return g_fail ? 1 : 0;
}
