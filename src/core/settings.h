// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// User settings. JSON keys are the reference implementation's (overlay.py DEFAULTS), minus the FD ones
// (flight director = phase 3, not in this program).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/vec.h"

namespace birdie {

// The global hotkeys: action -> key combo ("Ctrl+Alt+Insert"; "" = none). The actions are app/hotkeys.h
// hotkey_actions(); the combos are checked there (parse_combo).
const std::map<std::string, std::string>& default_hotkeys();

struct Settings {
  double glide_deg = 3.0;
  int lead_ms = 0;
  double smooth_s = 0.15;
  double box_w = 0.6, box_h = 0.7;  // overlay window, fraction of the MSFS client area
  bool visible = true;
  double line_span_deg = 18.0;
  double line_gap_deg = 6.0;
  bool show_runway = true;
  bool show_bird = true;
  bool show_glide = true;
  bool show_horizon = true;
  double opacity = 1.0;
  double ladder_step_deg = 6.0;
  double ladder_max_deg = 30.0;
  bool show_speed = true;
  bool show_alt = true;
  bool show_vs = true;
  bool show_roll = true;
  bool show_hdg = true;
  bool show_gs = true;
  bool show_att = true;
  double hud_scale = 1.0;
  bool show_pitch_minor = true;
  bool show_fpa_num = true;
  bool show_vs_num = true;
  bool show_bird_ra = true;  // radio altitude under the bird (new in Birdie; the reference has no such key)
  double bird_ra_below_ft = 500.0;  // ... shown only below this radio altitude
  bool xwind_mode = false;  // crosswind mode: shorter speed / altitude tapes moved outward, no VS needle (Birdie's own)
  bool show_flare = true;
  double flare_tol_deg = 0.3;
  // The flare cue (core/model.cpp flare_target): shown below flare_ra_ft; the target path goes from the approach
  // angle at flare_hi_ft to flare_end_deg at flare_lo_ft.
  double flare_ra_ft = 50.0, flare_hi_ft = 40.0, flare_lo_ft = 10.0, flare_end_deg = -0.5;
  // What "++" means. true (Birdie, user 11/17): still steeper than the target, keep pulling; flatter than it shows
  // nothing. false = the reference's: ++ when flatter, nothing when steeper, so the cue vanished from ~30 ft (the
  // target flattens before the pilot does) until the flare was nearly done. The parity test uses false.
  bool flare_pull = true;
  // Touchdown zone marks on the runway outline (core/runway.h touchdown_marks): rule "icao" or "faa"; what:
  // "aim" (aiming point + where the zone ends), "full" (+ the zone bars), "end" (only where it ends).
  bool show_tdz = true;
  std::string tdz_rule = "icao", tdz_style = "aim";
  std::string language;  // "" = follow Windows, "zh-CN", "en"
  std::string ui_style = "tool";  // the panel's look: "tool", "light", "dusk" (birdie.css)
  // Where the HUD's camera comes from (core/camera.h): "chaseplane" (ChasePlane's L-vars) or "msfs" (the sim's own
  // Camera API, beta: if an MSFS update breaks it, the user switches back).
  std::string camera_source = "chaseplane";
  std::map<std::string, std::string> hotkeys = default_hotkeys();
  // Showing and hiding by itself. On the ground the HUD waits until the ground speed passes auto_show_kt (the
  // take-off roll); up high it hides once the radio altitude has stayed above auto_hide_ft for auto_hide_s, and comes
  // back as soon as it drops below.
  bool auto_show = true;
  double auto_show_kt = 40.0;
  bool auto_hide = false;
  double auto_hide_ft = 2500.0;
  double auto_hide_s = 30.0;
  // The HUD repaints with the display (at most this often, Hz) and moves the last sim state on to the moment of
  // painting (core/model.h set_predict); 0 = repaint only when the sim sends a frame (before 11/24).
  int hud_max_hz = 120;

  // ---- the HUD's look, all of it adjustable (the Fine tuning page of the panel). Defaults = the Airbus HUD as drawn
  // before these existed (the parity tests rely on that).
  // Layout. dx / dy in degrees on the HUD plane (+ = right / down), size = scale about the element's own anchor,
  // len = tape length factor. lay_spread moves the side elements (tapes, VS, wind) apart (> 1) or together (< 1).
  double lay_spread = 1.0;
  double spd_dx = 0, spd_dy = 0, spd_len = 1.0, spd_size = 1.0;
  double alt_dx = 0, alt_dy = 0, alt_len = 1.0, alt_size = 1.0;
  double vs_dx = 0, vs_dy = 0, vs_size = 1.0;
  double wind_dx = 0, wind_dy = 0, wind_size = 1.0;
  double roll_dy = 0, roll_size = 1.0;
  double att_size = 1.0;
  double bird_size = 1.0;
  double ladder_w = 1.0;  // pitch rung length
  double hdg_dy = 0;      // the heading arc / tape (not the marks on the horizon)
  double text_size = 1.0, line_w = 1.0;
  // Element styles (hud/scene.cpp): which drawing of each symbol.
  std::string sty_fpv = "airbus";     // airbus boeing fighter minimal ring cross
  std::string sty_speed = "airbus";   // airbus boeing box
  std::string sty_alt = "airbus";     // airbus boeing box
  std::string sty_vs = "needle";      // needle digital
  std::string sty_ladder = "airbus";  // airbus boeing fighter minimal
  std::string sty_hdg = "horizon";    // horizon arc tape
  std::string sty_roll = "airbus";    // airbus (A320 HUD) boeing bottom; a32nx (FlyByWire PFD, parity only)
  bool show_accel = false;            // flight path acceleration caret beside the bird (Boeing HGS)
  std::string hud_font = "bahnschrift";  // bahnschrift consolas segoe narrow
  // Colour and finish.
  std::string hud_color = "#00ff00";
  double hud_outline = 1.0;  // the dark edge under every stroke (0 = none)
  double hud_glow = 0.0;     // a soft halo in the HUD colour (0 = none)
  // Showing and hiding: none fade iris boot scan assemble; anim_time scales the duration.
  std::string anim_in = "fade", anim_out = "fade";
  double anim_time = 1.0;
  // The whole-HUD preset last applied (airbus boeing fighter minimal), "custom" once something is changed by hand.
  std::string hud_preset = "airbus";
  // Terrain (terrain/, the panel's Terrain page): a see-through HUD-green surface on the slopes and the skyline, from
  // the Copernicus DEM. Off by default: it needs data from the internet. terrain_look: "fill" (slopes + skyline),
  // "sky" (the skyline only), "ridges" (slopes + skyline + the ridges in front of it). terrain_strength scales the
  // fill. Streaming fetches the tiles around the aircraft while the HUD is switched on, into a cache of at most
  // terrain_cache_gb (the least recently used go first); terrain_res 30 or 90 m. terrain_dir "" = the default folder
  // (%LOCALAPPDATA%\Birdie\dem). terrain_notes: say on the HUD when streaming starts and is done.
  bool terrain_on = false;
  std::string terrain_look = "fill";
  double terrain_strength = 1.0;
  bool terrain_stream = true;
  int terrain_res = 30;
  double terrain_cache_gb = 2.0;
  std::string terrain_dir;
  bool terrain_notes = true;

  // Birdie's settings window (tray: Open panel): top-left in physical screen px, kNoPos = not placed yet.
  static constexpr int kNoPos = -100000;
  int panel_win_x = kNoPos, panel_win_y = kNoPos;
  // The MSFS Community folder the user picked for the in-sim panel (app/ingame.h); "" = found from UserCfg.opt.
  std::string community_dir;

  // RADIO HEIGHT reads several ft on the ground (gear not counted); the reading on the ground is subtracted. Kept
  // per aircraft (SimConnect TITLE). Measured automatically the first time an aircraft stands still on the ground;
  // the tray can redo or clear it. (The reference had one global ra_offset_ft.)
  struct RaCal {
    double ft = 0.0;
    std::string note;
  };
  std::map<std::string, RaCal> ra_by_aircraft;

  // The MSFS camera source's origin offset per aircraft (TITLE), core/camera.h msfs_origin_offset: body frame m
  // (x right, y up, z forward). Measured automatically while the aircraft stands still; the panel can redo it.
  std::map<std::string, Vec3> cam_origin_by_aircraft;

  // HUD border (core/border.h, docs/HUD_BORDER.md): traced on the screen, kept per aircraft (TITLE) and seat; a seat
  // without its own uses the other one mirrored. pts in HUD-plane degrees; eye = where the eye was when traced
  // (lateral m + = right, then ChasePlane a_y, a_z), to tell when the view moved.
  struct BorderPoly {
    std::vector<Vec2> pts;
    Vec3 eye{};
  };
  struct BorderSeats {
    std::optional<BorderPoly> left, right;
  };
  std::map<std::string, BorderSeats> border_by_aircraft;
  // The terrain's own border (the panel's Terrain page): the same kind of trace, kept the same way. Where an aircraft
  // has none, the terrain stays inside the HUD border (or the combiner).
  std::map<std::string, BorderSeats> terrain_border_by_aircraft;
};

// The whole-HUD presets: element styles, layout, colour (not the symbol switches, except "minimal"). False if unknown.
bool apply_preset(Settings& s, const std::string& name);
// Every key that belongs to the HUD's look (presets, layout, styles, colour): changing one by hand makes the preset
// "custom"; "reset layout" puts the layout ones back.
bool is_look_key(const std::string& key);
void reset_layout(Settings& s);
// Sets one look key from the panel (numbers clamped to their range and snapped to 0.01; strings only from their
// list; hud_color "#rrggbb"). False = not a look key or a wrong value (unchanged).
bool set_look_value(Settings& s, const std::string& key, const nlohmann::json& v);
// Every look key with its value, for the panel.
void look_to_json(const Settings& s, nlohmann::json& out);

// Missing keys keep their defaults; unknown keys are ignored; wrong types are skipped.
Settings settings_from_json(const nlohmann::json& j);
nlohmann::json settings_to_json(const Settings& s);
// One seat's border: {"pts": [[x, y], ...], "eye": [x, y, z]}; nullopt if malformed (too few points, wrong types).
std::optional<Settings::BorderPoly> border_from_json(const nlohmann::json& v);
nlohmann::json border_to_json(const Settings::BorderPoly& b);

}  // namespace birdie
