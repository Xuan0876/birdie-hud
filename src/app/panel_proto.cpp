// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/panel_proto.h"

#include <algorithm>
#include <cmath>

#include "app/hotkeys.h"

namespace birdie::app {
namespace {

struct BoolKey {
  const char* key;
  bool Settings::*field;
};
const BoolKey kBools[] = {
    {"visible", &Settings::visible},          {"show_bird", &Settings::show_bird},
    {"show_fpa_num", &Settings::show_fpa_num}, {"show_vs_num", &Settings::show_vs_num},
    {"show_flare", &Settings::show_flare},    {"show_glide", &Settings::show_glide},
    {"show_horizon", &Settings::show_horizon}, {"show_hdg", &Settings::show_hdg},
    {"show_pitch_minor", &Settings::show_pitch_minor}, {"show_runway", &Settings::show_runway},
    {"show_att", &Settings::show_att},        {"show_speed", &Settings::show_speed},
    {"show_alt", &Settings::show_alt},        {"show_vs", &Settings::show_vs},
    {"show_roll", &Settings::show_roll},      {"show_gs", &Settings::show_gs},
    {"show_bird_ra", &Settings::show_bird_ra},  {"xwind_mode", &Settings::xwind_mode},
    {"auto_show", &Settings::auto_show},      {"auto_hide", &Settings::auto_hide},
    {"show_tdz", &Settings::show_tdz},
    {"terrain_on", &Settings::terrain_on},    {"terrain_stream", &Settings::terrain_stream},
    {"terrain_notes", &Settings::terrain_notes},
};

struct NumKey {
  const char* key;
  double Settings::*field;
  double lo, hi, step;  // step: rounding (0 = none)
};
const NumKey kNums[] = {
    {"glide_deg", &Settings::glide_deg, 0.5, 10.0, 0.1},
    {"hud_scale", &Settings::hud_scale, 0.5, 1.5, 0.01},
    {"opacity", &Settings::opacity, 0.15, 1.0, 0.01},
    {"flare_tol_deg", &Settings::flare_tol_deg, 0.1, 2.0, 0.1},
    {"ladder_step_deg", &Settings::ladder_step_deg, 0.0, 30.0, 0.5},
    {"bird_ra_below_ft", &Settings::bird_ra_below_ft, 50.0, 2500.0, 10.0},
    {"auto_show_kt", &Settings::auto_show_kt, 5.0, 150.0, 1.0},
    {"auto_hide_ft", &Settings::auto_hide_ft, 200.0, 20000.0, 50.0},
    {"auto_hide_s", &Settings::auto_hide_s, 0.0, 600.0, 1.0},
    {"terrain_strength", &Settings::terrain_strength, 0.2, 2.0, 0.05},
    {"terrain_cache_gb", &Settings::terrain_cache_gb, 0.5, 64.0, 0.5},
};

double snap(double v, double lo, double hi, double step) {
  v = std::clamp(v, lo, hi);
  if (step > 0) v = std::round(v / step) * step;
  return std::round(v * 1e6) / 1e6;  // no 0.30000000000000004 in settings.json
}

}  // namespace

nlohmann::json panel_settings(const Settings& s) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& b : kBools) j[b.key] = s.*b.field;
  for (const auto& n : kNums) j[n.key] = s.*n.field;
  j["lead_ms"] = s.lead_ms;
  j["language"] = s.language;
  j["ui_style"] = s.ui_style;
  j["camera_source"] = s.camera_source;
  j["hotkeys"] = s.hotkeys;
  j["terrain_look"] = s.terrain_look;
  j["terrain_res"] = s.terrain_res;
  look_to_json(s, j);
  return j;
}

bool apply_panel_set(Settings& s, const std::string& key, const nlohmann::json& v) {
  for (const auto& b : kBools) {
    if (key != b.key) continue;
    if (!v.is_boolean()) return false;
    s.*b.field = v.get<bool>();
    return true;
  }
  for (const auto& n : kNums) {
    if (key != n.key) continue;
    if (!v.is_number()) return false;
    const double x = v.get<double>();
    if (!std::isfinite(x)) return false;
    s.*n.field = snap(x, n.lo, n.hi, n.step);
    return true;
  }
  if (key == "lead_ms") {
    if (!v.is_number()) return false;
    const double x = v.get<double>();
    if (!std::isfinite(x)) return false;
    s.lead_ms = static_cast<int>(std::lround(std::clamp(x, -100.0, 300.0)));
    return true;
  }
  if (key == "language") {
    if (!v.is_string()) return false;
    const auto l = v.get<std::string>();
    if (l != "" && l != "zh-CN" && l != "en") return false;
    s.language = l;
    return true;
  }
  if (set_look_value(s, key, v)) {
    if (is_look_key(key)) s.hud_preset = "custom";  // changed by hand
    return true;
  }
  if (key.rfind("hotkey.", 0) == 0) {  // "hotkey.toggle": "Ctrl+Alt+Insert", "" = none
    auto it = s.hotkeys.find(key.substr(7));
    if (it == s.hotkeys.end() || !v.is_string()) return false;
    const std::string raw = v.get<std::string>();
    const std::string c = raw.empty() ? std::string() : canonical_combo(raw);
    if (!raw.empty() && c.empty()) return false;
    if (!c.empty())
      for (auto& [other, combo] : s.hotkeys)  // one combo, one action: the one it had loses it
        if (other != it->first && canonical_combo(combo) == c) combo.clear();
    it->second = c;
    return true;
  }
  if (key == "terrain_look") {
    if (!v.is_string()) return false;
    const auto l = v.get<std::string>();
    if (l != "fill" && l != "sky" && l != "ridges") return false;
    s.terrain_look = l;
    return true;
  }
  if (key == "terrain_res") {
    if (!v.is_number() || (v.get<double>() != 30 && v.get<double>() != 90)) return false;
    s.terrain_res = static_cast<int>(v.get<double>());
    return true;
  }
  if (key == "camera_source") {
    if (!v.is_string()) return false;
    const auto l = v.get<std::string>();
    if (l != "chaseplane" && l != "msfs") return false;
    s.camera_source = l;
    return true;
  }
  if (key == "ui_style") {
    if (!v.is_string()) return false;
    const auto l = v.get<std::string>();
    if (l != "tool" && l != "light" && l != "dusk") return false;
    s.ui_style = l;
    return true;
  }
  return false;
}

}  // namespace birdie::app
