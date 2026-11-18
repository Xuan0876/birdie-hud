// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// One sample of the simulator state, as read from SimConnect every sim frame.
#pragma once

#include <array>
#include <string_view>

namespace birdie {

struct SimState {
  double lat = 0, lon = 0;
  double hdg_true = 0;
  double pitch_deg = 0;  // SimConnect sign: negative = nose up
  double bank_deg = 0;   // SimConnect sign: positive = left wing down
  double trk_true = 0;
  double gs_kt = 0;
  double vy_fps = 0;  // VELOCITY WORLD Y (VERTICAL SPEED lags in the flare)
  double vs_fpm = 0;
  double alt_ft = 0;
  double agl_ft = 0;
  double on_ground = 0;
  double cp_mode = 0;  // ChasePlane L-vars
  double cp_pitch = 0;
  double cp_yaw = 0;
  double cp_roll = 0;
  double cp_zoom = 0;
  double cp_eye_x = 0, cp_eye_y = 0, cp_eye_z = 0;  // L:p42_cp_a_x/y/z: eyepoint anchor + view offset, m
  double cp_off_x = 0;  // L:p42_cp_r_x: the view's lateral offset from the anchor (+ right); see geo.h eye_lateral_m
  double ias_kt = 0;
  double mach = 0;
  double ind_alt_ft = 0;
  double radio_alt_ft = 0;  // after the ground calibration offset
  double baro_mb = 0;
  double hdg_mag = 0;
  double wind_dir = 0;  // true, from
  double wind_kt = 0;
  double beta_deg = 0;
  double ra_raw_ft = 0;  // RADIO HEIGHT as read (not part of the SimConnect definition)
};

struct VarDef {
  const char* key;     // name in settings/fixtures (= the reference's VARS keys)
  const char* simvar;  // SimConnect name
  const char* unit;
  double SimState::*field;
};

// Order = SimConnect data definition order (all FLOAT64). Keys the reference fixtures lack: not_in_reference().
inline constexpr std::array<VarDef, 30> kVars{{
    {"lat", "PLANE LATITUDE", "degrees", &SimState::lat},
    {"lon", "PLANE LONGITUDE", "degrees", &SimState::lon},
    {"hdg_true", "PLANE HEADING DEGREES TRUE", "degrees", &SimState::hdg_true},
    {"pitch_deg", "PLANE PITCH DEGREES", "degrees", &SimState::pitch_deg},
    {"bank_deg", "PLANE BANK DEGREES", "degrees", &SimState::bank_deg},
    {"trk_true", "GPS GROUND TRUE TRACK", "degrees", &SimState::trk_true},
    {"gs_kt", "GROUND VELOCITY", "knots", &SimState::gs_kt},
    {"vy_fps", "VELOCITY WORLD Y", "feet per second", &SimState::vy_fps},
    {"vs_fpm", "VERTICAL SPEED", "feet per minute", &SimState::vs_fpm},
    {"alt_ft", "PLANE ALTITUDE", "feet", &SimState::alt_ft},
    {"agl_ft", "PLANE ALT ABOVE GROUND", "feet", &SimState::agl_ft},
    {"on_ground", "SIM ON GROUND", "bool", &SimState::on_ground},
    {"cp_mode", "L:p42_cp_mode", "number", &SimState::cp_mode},
    {"cp_pitch", "L:p42_cp_r_pitch", "number", &SimState::cp_pitch},
    {"cp_yaw", "L:p42_cp_r_yaw", "number", &SimState::cp_yaw},
    {"cp_roll", "L:p42_cp_r_roll", "number", &SimState::cp_roll},
    {"cp_zoom", "L:p42_cp_r_zoom", "number", &SimState::cp_zoom},
    {"cp_eye_x", "L:p42_cp_a_x", "number", &SimState::cp_eye_x},
    {"cp_eye_y", "L:p42_cp_a_y", "number", &SimState::cp_eye_y},
    {"cp_eye_z", "L:p42_cp_a_z", "number", &SimState::cp_eye_z},
    {"ias_kt", "AIRSPEED INDICATED", "knots", &SimState::ias_kt},
    {"mach", "AIRSPEED MACH", "mach", &SimState::mach},
    {"ind_alt_ft", "INDICATED ALTITUDE", "feet", &SimState::ind_alt_ft},
    {"radio_alt_ft", "RADIO HEIGHT", "feet", &SimState::radio_alt_ft},
    {"baro_mb", "KOHLSMAN SETTING MB", "millibars", &SimState::baro_mb},
    {"hdg_mag", "PLANE HEADING DEGREES MAGNETIC", "degrees", &SimState::hdg_mag},
    {"wind_dir", "AMBIENT WIND DIRECTION", "degrees", &SimState::wind_dir},
    {"wind_kt", "AMBIENT WIND VELOCITY", "knots", &SimState::wind_kt},
    {"beta_deg", "INCIDENCE BETA", "degrees", &SimState::beta_deg},
    {"cp_off_x", "L:p42_cp_r_x", "number", &SimState::cp_off_x},
}};

// Added after the Python reference froze: its fixtures have no such column (read as 0).
inline bool not_in_reference(const char* key) { return std::string_view(key) == "cp_off_x"; }

}  // namespace birdie
