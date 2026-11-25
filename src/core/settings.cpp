// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "core/settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "core/border.h"

namespace birdie {

const std::map<std::string, std::string>& default_hotkeys() {
  static const std::map<std::string, std::string> d = {
      {"toggle", "Ctrl+Alt+Insert"}, {"xwind", "Ctrl+Alt+Shift+Insert"}, {"glide_up", "Ctrl+Alt+PgUp"},
      {"glide_down", "Ctrl+Alt+PgDn"}, {"lead_up", "Ctrl+Alt+Home"},     {"lead_down", "Ctrl+Alt+End"},
      {"bright_up", ""},  {"bright_down", ""}, {"size_up", ""}, {"size_down", ""}, {"preset_next", ""},
      {"anim_test", ""},  {"panel", ""},       {"quit", "Ctrl+Alt+Shift+End"},
  };
  return d;
}

namespace {

template <class F>
void each_field(Settings& s, F&& f) {
  f("glide_deg", s.glide_deg);
  f("lead_ms", s.lead_ms);
  f("smooth_s", s.smooth_s);
  f("box_w", s.box_w);
  f("box_h", s.box_h);
  f("visible", s.visible);
  f("line_span_deg", s.line_span_deg);
  f("line_gap_deg", s.line_gap_deg);
  f("show_runway", s.show_runway);
  f("show_bird", s.show_bird);
  f("show_glide", s.show_glide);
  f("show_horizon", s.show_horizon);
  f("opacity", s.opacity);
  f("ladder_step_deg", s.ladder_step_deg);
  f("ladder_max_deg", s.ladder_max_deg);
  f("show_speed", s.show_speed);
  f("show_alt", s.show_alt);
  f("show_vs", s.show_vs);
  f("show_roll", s.show_roll);
  f("show_hdg", s.show_hdg);
  f("show_gs", s.show_gs);
  f("show_att", s.show_att);
  f("hud_scale", s.hud_scale);
  f("show_pitch_minor", s.show_pitch_minor);
  f("show_fpa_num", s.show_fpa_num);
  f("show_vs_num", s.show_vs_num);
  f("show_bird_ra", s.show_bird_ra);
  f("bird_ra_below_ft", s.bird_ra_below_ft);
  f("xwind_mode", s.xwind_mode);
  f("show_flare", s.show_flare);
  f("flare_tol_deg", s.flare_tol_deg);
  f("flare_ra_ft", s.flare_ra_ft);
  f("flare_hi_ft", s.flare_hi_ft);
  f("flare_lo_ft", s.flare_lo_ft);
  f("flare_end_deg", s.flare_end_deg);
  f("flare_pull", s.flare_pull);
  f("show_tdz", s.show_tdz);
  f("tdz_rule", s.tdz_rule);
  f("tdz_style", s.tdz_style);
  f("language", s.language);
  f("ui_style", s.ui_style);
  f("camera_source", s.camera_source);
  f("auto_show", s.auto_show);
  f("auto_show_kt", s.auto_show_kt);
  f("auto_hide", s.auto_hide);
  f("auto_hide_ft", s.auto_hide_ft);
  f("auto_hide_s", s.auto_hide_s);
  f("lay_spread", s.lay_spread);
  f("spd_dx", s.spd_dx);
  f("spd_dy", s.spd_dy);
  f("spd_len", s.spd_len);
  f("spd_size", s.spd_size);
  f("alt_dx", s.alt_dx);
  f("alt_dy", s.alt_dy);
  f("alt_len", s.alt_len);
  f("alt_size", s.alt_size);
  f("vs_dx", s.vs_dx);
  f("vs_dy", s.vs_dy);
  f("vs_size", s.vs_size);
  f("wind_dx", s.wind_dx);
  f("wind_dy", s.wind_dy);
  f("wind_size", s.wind_size);
  f("roll_dy", s.roll_dy);
  f("roll_size", s.roll_size);
  f("att_size", s.att_size);
  f("bird_size", s.bird_size);
  f("ladder_w", s.ladder_w);
  f("hdg_dy", s.hdg_dy);
  f("text_size", s.text_size);
  f("line_w", s.line_w);
  f("sty_fpv", s.sty_fpv);
  f("sty_speed", s.sty_speed);
  f("sty_alt", s.sty_alt);
  f("sty_vs", s.sty_vs);
  f("sty_ladder", s.sty_ladder);
  f("sty_hdg", s.sty_hdg);
  f("sty_roll", s.sty_roll);
  f("show_accel", s.show_accel);
  f("hud_font", s.hud_font);
  f("hud_color", s.hud_color);
  f("hud_outline", s.hud_outline);
  f("hud_glow", s.hud_glow);
  f("anim_in", s.anim_in);
  f("anim_out", s.anim_out);
  f("anim_time", s.anim_time);
  f("hud_preset", s.hud_preset);
  f("panel_win_x", s.panel_win_x);
  f("panel_win_y", s.panel_win_y);
  f("community_dir", s.community_dir);
  f("terrain_on", s.terrain_on);
  f("terrain_look", s.terrain_look);
  f("terrain_strength", s.terrain_strength);
  f("terrain_stream", s.terrain_stream);
  f("terrain_res", s.terrain_res);
  f("terrain_cache_gb", s.terrain_cache_gb);
  f("terrain_dir", s.terrain_dir);
  f("terrain_notes", s.terrain_notes);
  f("tint_on", s.tint_on);
  f("tint_color", s.tint_color);
  f("tint_alpha", s.tint_alpha);
  f("tint_edge", s.tint_edge);
}

void read(const nlohmann::json& v, double& out) {
  if (v.is_number()) out = v.get<double>();
}
void read(const nlohmann::json& v, int& out) {
  if (v.is_number()) out = static_cast<int>(v.get<double>());
}
void read(const nlohmann::json& v, bool& out) {
  if (v.is_boolean()) out = v.get<bool>();
  else if (v.is_number()) out = v.get<double>() != 0;
}
void read(const nlohmann::json& v, std::string& out) {
  if (v.is_string()) out = v.get<std::string>();
}

// ---- the HUD's look: ranges and choices
struct LookNum {
  const char* key;
  double Settings::*field;
  double lo, hi, def;
  bool layout;  // reset by "reset layout"
};
const LookNum kLookNums[] = {
    {"lay_spread", &Settings::lay_spread, 0.6, 1.4, 1.0, true},
    {"spd_dx", &Settings::spd_dx, -10, 10, 0, true},     {"spd_dy", &Settings::spd_dy, -10, 10, 0, true},
    {"spd_len", &Settings::spd_len, 0.4, 1.6, 1.0, true}, {"spd_size", &Settings::spd_size, 0.6, 1.6, 1.0, true},
    {"alt_dx", &Settings::alt_dx, -10, 10, 0, true},     {"alt_dy", &Settings::alt_dy, -10, 10, 0, true},
    {"alt_len", &Settings::alt_len, 0.4, 1.6, 1.0, true}, {"alt_size", &Settings::alt_size, 0.6, 1.6, 1.0, true},
    {"vs_dx", &Settings::vs_dx, -10, 10, 0, true},       {"vs_dy", &Settings::vs_dy, -10, 10, 0, true},
    {"vs_size", &Settings::vs_size, 0.6, 1.6, 1.0, true},
    {"wind_dx", &Settings::wind_dx, -10, 10, 0, true},   {"wind_dy", &Settings::wind_dy, -10, 10, 0, true},
    {"wind_size", &Settings::wind_size, 0.6, 1.6, 1.0, true},
    {"roll_dy", &Settings::roll_dy, -10, 10, 0, true},   {"roll_size", &Settings::roll_size, 0.6, 1.6, 1.0, true},
    {"att_size", &Settings::att_size, 0.5, 2.0, 1.0, true},
    {"bird_size", &Settings::bird_size, 0.5, 2.0, 1.0, true},
    {"ladder_w", &Settings::ladder_w, 0.5, 2.0, 1.0, true},
    {"hdg_dy", &Settings::hdg_dy, -10, 10, 0, true},
    {"text_size", &Settings::text_size, 0.7, 1.5, 1.0, true}, {"line_w", &Settings::line_w, 0.5, 2.0, 1.0, true},
    {"hud_outline", &Settings::hud_outline, 0.0, 1.5, 1.0, false},
    {"hud_glow", &Settings::hud_glow, 0.0, 1.0, 0.0, false},
    {"anim_time", &Settings::anim_time, 0.3, 3.0, 1.0, false},
    {"flare_ra_ft", &Settings::flare_ra_ft, 20.0, 100.0, 50.0, false},
    {"flare_hi_ft", &Settings::flare_hi_ft, 15.0, 80.0, 40.0, false},
    {"flare_lo_ft", &Settings::flare_lo_ft, 0.0, 30.0, 10.0, false},
    {"flare_end_deg", &Settings::flare_end_deg, -2.0, 1.0, -0.5, false},
};
struct LookStr {
  const char* key;
  std::string Settings::*field;
  std::vector<const char*> allowed;  // empty = a colour "#rrggbb"
};
const std::vector<const char*> kAnims = {"none", "fade", "iris", "boot", "scan", "assemble"};
const LookStr kLookStrs[] = {
    {"sty_fpv", &Settings::sty_fpv, {"airbus", "boeing", "fighter", "minimal", "ring", "cross"}},
    {"sty_speed", &Settings::sty_speed, {"airbus", "boeing", "box"}},
    {"sty_alt", &Settings::sty_alt, {"airbus", "boeing", "box"}},
    {"sty_vs", &Settings::sty_vs, {"needle", "digital"}},
    {"sty_ladder", &Settings::sty_ladder, {"airbus", "boeing", "fighter", "minimal"}},
    {"sty_hdg", &Settings::sty_hdg, {"horizon", "arc", "tape"}},
    // "a32nx" (the reference's FlyByWire PFD roll scale) still draws for the parity test but is not offered: user 11/17,
    // "not realistic"; a settings file with it falls back to airbus
    {"sty_roll", &Settings::sty_roll, {"airbus", "boeing", "bottom"}},
    {"hud_font", &Settings::hud_font, {"bahnschrift", "consolas", "segoe", "narrow"}},
    {"hud_color", &Settings::hud_color, {}},
    {"anim_in", &Settings::anim_in, kAnims},
    {"anim_out", &Settings::anim_out, kAnims},
    {"hud_preset", &Settings::hud_preset, {"airbus", "boeing", "fighter", "minimal", "custom"}},
    {"tdz_rule", &Settings::tdz_rule, {"icao", "faa"}},
    {"tdz_style", &Settings::tdz_style, {"aim", "full", "end"}},
};

bool is_hex_color(const std::string& c) {
  if (c.size() != 7 || c[0] != '#') return false;
  for (size_t i = 1; i < 7; ++i)
    if (!std::isxdigit(static_cast<unsigned char>(c[i]))) return false;
  return true;
}

bool str_ok(const LookStr& d, const std::string& v) {
  if (d.allowed.empty()) return is_hex_color(v);
  for (const char* a : d.allowed)
    if (v == a) return true;
  return false;
}

bool starts(const char* s, const char* prefix) { return std::string(s).rfind(prefix, 0) == 0; }

// settings.json is hand-editable: keep the look inside what the drawing code expects
void sanitize_look(Settings& s) {
  for (const auto& n : kLookNums) {
    double& v = s.*n.field;
    v = std::isfinite(v) ? std::clamp(v, n.lo, n.hi) : n.def;
  }
  const Settings def;
  for (const auto& d : kLookStrs)
    if (!str_ok(d, s.*d.field)) s.*d.field = def.*d.field;
}

}  // namespace

bool tint_color_ok(const std::string& c) { return c == "hud" || is_hex_color(c); }
bool tint_edge_ok(const std::string& e) { return e == "hard" || e == "feather" || e == "wide" || e == "line"; }

bool is_look_key(const std::string& key) {
  if (key == "show_accel") return true;
  // animations, the flare cue and the touchdown zone are not part of a preset
  for (const char* other : {"anim", "flare", "tdz", "hud_preset"})
    if (starts(key.c_str(), other)) return false;
  for (const auto& n : kLookNums)
    if (key == n.key) return true;
  for (const auto& d : kLookStrs)
    if (key == d.key) return true;
  return false;
}

void reset_layout(Settings& s) {
  for (const auto& n : kLookNums)
    if (n.layout) s.*n.field = n.def;
}

bool set_look_value(Settings& s, const std::string& key, const nlohmann::json& v) {
  for (const auto& n : kLookNums) {
    if (key != n.key) continue;
    if (!v.is_number()) return false;
    const double x = v.get<double>();
    if (!std::isfinite(x)) return false;
    s.*n.field = std::round(std::clamp(x, n.lo, n.hi) * 100.0) / 100.0;
    return true;
  }
  for (const auto& d : kLookStrs) {
    if (key != d.key) continue;
    if (!v.is_string() || !str_ok(d, v.get<std::string>())) return false;
    s.*d.field = v.get<std::string>();
    return true;
  }
  if (key == "show_accel") {
    if (!v.is_boolean()) return false;
    s.show_accel = v.get<bool>();
    return true;
  }
  return false;
}

void look_to_json(const Settings& s, nlohmann::json& out) {
  for (const auto& n : kLookNums) out[n.key] = s.*n.field;
  for (const auto& d : kLookStrs) out[d.key] = s.*d.field;
  out["show_accel"] = s.show_accel;
}

bool apply_preset(Settings& s, const std::string& name) {
  if (name != "airbus" && name != "boeing" && name != "fighter" && name != "minimal") return false;
  // start from the Airbus HUD: layout, styles, colour and finish back to their defaults (animations stay)
  const Settings def;
  reset_layout(s);
  for (const auto& d : kLookStrs)
    if (is_look_key(d.key)) s.*d.field = def.*d.field;
  s.show_accel = false;
  s.hud_outline = def.hud_outline;
  s.hud_glow = def.hud_glow;
  if (name == "boeing") {
    // Boeing HGS: boxed speed / altitude readouts on their tapes, VS as a number, the acceleration caret beside a
    // longer-winged flight path symbol, a compass arc low in the view, the bank pointer under bare ticks
    s.sty_fpv = "boeing", s.sty_speed = "boeing", s.sty_alt = "boeing", s.sty_vs = "digital";
    s.sty_ladder = "boeing", s.sty_hdg = "arc", s.sty_roll = "boeing";
    s.show_accel = true;
    s.lay_spread = 1.05, s.spd_len = 0.9, s.alt_len = 0.9;
    s.hud_color = "#3cff74";
  } else if (name == "fighter") {
    // fighter jet: no tapes, boxed numbers; heading tape on top, bank scale at the bottom, bent dashed rungs
    s.sty_fpv = "fighter", s.sty_speed = "box", s.sty_alt = "box", s.sty_vs = "digital";
    s.sty_ladder = "fighter", s.sty_hdg = "tape", s.sty_roll = "bottom";
    s.lay_spread = 0.85, s.ladder_w = 1.15;
    s.hud_font = "consolas", s.hud_color = "#7dff8c", s.hud_glow = 0.45, s.hud_outline = 0.6;
  } else if (name == "minimal") {
    // the least that still flies: bird, horizon, glide line, runway; numbers small and out of the way
    s.sty_fpv = "minimal", s.sty_speed = "box", s.sty_alt = "box", s.sty_vs = "digital", s.sty_ladder = "minimal";
    s.lay_spread = 1.15, s.text_size = 0.9, s.line_w = 0.85;
    s.show_roll = false, s.show_pitch_minor = false, s.show_hdg = false, s.show_att = false;
    s.hud_color = "#e8fff0", s.hud_outline = 0.8;
  }
  s.hud_preset = name;
  return true;
}

Settings settings_from_json(const nlohmann::json& j) {
  Settings s;
  if (!j.is_object()) return s;
  each_field(s, [&](const char* key, auto& field) {
    auto it = j.find(key);
    if (it != j.end()) read(*it, field);
  });
  if (s.terrain_look != "fill" && s.terrain_look != "sky" && s.terrain_look != "ridges") s.terrain_look = "fill";
  if (s.terrain_res != 90) s.terrain_res = 30;
  if (s.camera_source != "msfs") s.camera_source = "chaseplane";
  if (!std::isfinite(s.terrain_strength)) s.terrain_strength = 1.0;
  s.terrain_strength = std::clamp(s.terrain_strength, 0.2, 2.0);
  if (!std::isfinite(s.terrain_cache_gb)) s.terrain_cache_gb = 2.0;
  s.terrain_cache_gb = std::clamp(s.terrain_cache_gb, 0.5, 64.0);
  if (!tint_color_ok(s.tint_color)) s.tint_color = "hud";
  if (!tint_edge_ok(s.tint_edge)) s.tint_edge = "feather";
  s.tint_alpha = std::isfinite(s.tint_alpha) ? std::clamp(s.tint_alpha, 0.0, 0.5) : 0.10;
  auto ra = j.find("ra_by_aircraft");
  if (ra != j.end() && ra->is_object()) {
    for (const auto& [title, v] : ra->items()) {
      if (!v.is_object()) continue;
      Settings::RaCal c;
      if (auto f = v.find("ft"); f != v.end()) read(*f, c.ft);
      if (auto n = v.find("note"); n != v.end()) read(*n, c.note);
      if (std::abs(c.ft) < 100) s.ra_by_aircraft[title] = c;
    }
  }
  if (auto co = j.find("cam_origin_by_aircraft"); co != j.end() && co->is_object()) {
    for (const auto& [title, v] : co->items()) {
      if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) continue;
      const Vec3 o{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
      if (std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z) && std::abs(o.x) < 200 && std::abs(o.y) < 200 &&
          std::abs(o.z) < 200)
        s.cam_origin_by_aircraft[title] = o;
    }
  }
  if (auto hk = j.find("hotkeys"); hk != j.end() && hk->is_object()) {
    for (const auto& [action, v] : hk->items()) {
      auto it = s.hotkeys.find(action);
      if (it != s.hotkeys.end() && v.is_string() && v.get<std::string>().size() <= 40) it->second = v.get<std::string>();
    }
  }
  auto borders = [&](const char* name, std::map<std::string, Settings::BorderSeats>& out) {
    auto bd = j.find(name);
    if (bd == j.end() || !bd->is_object()) return;
    for (const auto& [title, v] : bd->items()) {
      if (!v.is_object()) continue;
      Settings::BorderSeats seats;
      for (const char* key : {"L", "R"}) {
        auto it = v.find(key);
        if (it == v.end()) continue;
        if (auto b = border_from_json(*it)) (key[0] == 'L' ? seats.left : seats.right) = std::move(*b);
      }
      if (seats.left || seats.right) out[title] = std::move(seats);
    }
  };
  borders("border_by_aircraft", s.border_by_aircraft);
  borders("terrain_border_by_aircraft", s.terrain_border_by_aircraft);
  sanitize_look(s);
  return s;
}

std::optional<Settings::BorderPoly> border_from_json(const nlohmann::json& v) {
  if (!v.is_object()) return std::nullopt;
  auto pts = v.find("pts");
  if (pts == v.end() || !pts->is_array()) return std::nullopt;
  Settings::BorderPoly b;
  for (const auto& p : *pts) {
    if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) return std::nullopt;
    b.pts.push_back({p[0].get<double>(), p[1].get<double>()});
  }
  if (!sanitize_border(b.pts)) return std::nullopt;
  if (auto e = v.find("eye"); e != v.end() && e->is_array() && e->size() == 3) {
    double xyz[3] = {};
    for (size_t i = 0; i < 3; ++i) read((*e)[i], xyz[i]);
    b.eye = {xyz[0], xyz[1], xyz[2]};
  }
  return b;
}

nlohmann::json border_to_json(const Settings::BorderPoly& b) {
  auto r4 = [](double x) { return std::round(x * 1e4) / 1e4; };
  nlohmann::json pts = nlohmann::json::array();
  for (const auto& p : b.pts) pts.push_back({r4(p.x), r4(p.y)});
  return {{"pts", pts}, {"eye", {r4(b.eye.x), r4(b.eye.y), r4(b.eye.z)}}};
}

nlohmann::json settings_to_json(const Settings& s0) {
  Settings s = s0;
  nlohmann::json j = nlohmann::json::object();
  each_field(s, [&](const char* key, auto& field) { j[key] = field; });
  nlohmann::json ra = nlohmann::json::object();
  for (const auto& [title, c] : s.ra_by_aircraft) ra[title] = {{"ft", c.ft}, {"note", c.note}};
  j["ra_by_aircraft"] = ra;
  if (!s.cam_origin_by_aircraft.empty()) {  // absent until the first one: settings files stay as they were
    nlohmann::json co = nlohmann::json::object();
    for (const auto& [title, o] : s.cam_origin_by_aircraft) co[title] = {o.x, o.y, o.z};
    j["cam_origin_by_aircraft"] = co;
  }
  j["hotkeys"] = s.hotkeys;
  auto borders = [&](const char* name, const std::map<std::string, Settings::BorderSeats>& in) {
    if (in.empty()) return;  // absent until the first border: settings files stay as they were
    nlohmann::json bd = nlohmann::json::object();
    for (const auto& [title, seats] : in) {
      nlohmann::json e = nlohmann::json::object();
      if (seats.left) e["L"] = border_to_json(*seats.left);
      if (seats.right) e["R"] = border_to_json(*seats.right);
      if (!e.empty()) bd[title] = e;
    }
    j[name] = bd;
  };
  borders("border_by_aircraft", s.border_by_aircraft);
  borders("terrain_border_by_aircraft", s.terrain_border_by_aircraft);
  return j;
}

}  // namespace birdie
