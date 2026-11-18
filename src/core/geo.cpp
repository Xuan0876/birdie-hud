// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "core/geo.h"

#include <algorithm>
#include <cmath>

namespace birdie {

namespace {

bool g_reference = false;
constexpr double kE2 = 6.69437999014e-3;  // WGS84 first eccentricity squared (kEarthRadius = semi-major axis)

// Metres per radian of longitude (x) and latitude (y) at `lat`.
Vec2 metres_per_rad(double lat) {
  const double c = std::cos(rad(lat));
  if (g_reference) return {kEarthRadius * c, kEarthRadius};  // sphere
  const double s = std::sin(rad(lat));
  const double w = 1.0 - kE2 * s * s;
  const double N = kEarthRadius / std::sqrt(w);              // prime vertical radius
  const double M = kEarthRadius * (1.0 - kE2) / (w * std::sqrt(w));  // meridional radius
  return {N * c, M};
}

}  // namespace

void set_reference_mode(bool on) { g_reference = on; }
bool reference_mode() { return g_reference; }

double eye_lateral_m(double a_x, double r_x) { return g_reference ? a_x : 2.0 * r_x - a_x; }

Vec2 enu(double lat0, double lon0, double lat, double lon) {
  const Vec2 k = metres_per_rad(lat0);
  return {rad(lon - lon0) * k.x, rad(lat - lat0) * k.y};
}

Vec2 offset_latlon(double lat, double lon, double e, double n) {
  const Vec2 k = metres_per_rad(lat);
  return {lat + deg(n / k.y), lon + deg(e / k.x)};
}

Mat3 aircraft_to_enu(double heading_true, double pitch_up, double bank_right) {
  const double cy = std::cos(rad(heading_true)), sy = std::sin(rad(heading_true));
  const double cp = std::cos(rad(pitch_up)), sp = std::sin(rad(pitch_up));
  const double cr = std::cos(rad(bank_right)), sr = std::sin(rad(bank_right));
  const Vec3 fwd{sy * cp, cy * cp, sp};
  const Vec3 right0{cy, -sy, 0.0};
  const Vec3 up0 = cross(right0, fwd);
  const Vec3 right = cr * right0 - sr * up0;
  const Vec3 up = sr * right0 + cr * up0;
  return Mat3::from_columns(right, up, fwd);
}

Mat3 aircraft_rotation(const SimState& s) { return aircraft_to_enu(s.hdg_true, -s.pitch_deg, -s.bank_deg); }

Mat3 camera_to_body(double yaw_right, double pitch_up, double roll) {
  const double cy = std::cos(rad(yaw_right)), sy = std::sin(rad(yaw_right));
  const double cp = std::cos(rad(pitch_up)), sp = std::sin(rad(pitch_up));
  const Vec3 fwd{sy * cp, sp, cy * cp};
  Vec3 right{cy, 0.0, -sy};
  Vec3 up = cross(fwd, right);
  if (roll != 0.0) {
    const double cr = std::cos(rad(roll)), sr = std::sin(rad(roll));
    const Vec3 r2 = cr * right - sr * up;
    const Vec3 u2 = sr * right + cr * up;
    right = r2;
    up = u2;
  }
  return Mat3::from_columns(right, up, fwd);
}

double vfov_deg(double zoom) {
  const double v_ref = 2 * deg(std::atan(720.0 / kFocalRefPx));
  return std::clamp(v_ref + kVfovPerZoom * (zoom - kZoomRef), 1.0, 170.0);
}

double focal_px(double zoom, double height) { return height / 2 / std::tan(rad(vfov_deg(zoom) / 2)); }

}  // namespace birdie
