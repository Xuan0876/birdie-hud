// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "core/runway.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/geo.h"

namespace birdie {

// TRUE_SLOPE: deg, + = rising along the primary (lower-numbered) direction. Checked on LOWI 08/26 (2026-11-07).
constexpr double kSlopeSign = 1.0;

Runway make_runway(std::string icao, double lat, double lon, double alt_m, double hdg, double length, double width,
                   std::string name, double slope) {
  Runway r;
  r.icao = std::move(icao);
  r.lat = lat;
  r.lon = lon;
  r.alt_m = alt_m;
  r.hdg = pymod(hdg, 360.0);
  r.length = length;
  r.width = width;
  r.name = std::move(name);
  r.slope = slope;
  r.facility_alt_m = alt_m;
  return r;
}

bool anchor_from_ground(Runway& rw, double lat, double lon, double ground_m) {
  if (!std::isfinite(ground_m)) return false;
  const Vec2 en = enu(rw.lat, rw.lon, lat, lon);
  const double h = rad(rw.hdg);
  const double along = en.x * std::sin(h) + en.y * std::cos(h);
  const double cross_ = en.x * std::cos(h) - en.y * std::sin(h);
  if (std::abs(along) > rw.length / 2 || std::abs(cross_) > rw.width / 2) return false;
  const double centre = ground_m - along * std::tan(rad(rw.slope));
  // first reading: take it; then follow gently (the real surface is not a perfect plane; called at 2 Hz -> ~1.5 s)
  rw.alt_m = rw.anchored ? rw.alt_m + 0.3 * (centre - rw.alt_m) : centre;
  rw.anchored = true;
  rw.alt_src = 4;  // kAltGround
  return true;
}

std::array<Runway::Corner, 4> Runway::corners() const {
  const double h = rad(hdg);
  const Vec2 u{std::sin(h), std::cos(h)};   // along, east/north
  const Vec2 r{std::cos(h), -std::sin(h)};  // right
  static constexpr double kAS[4][2] = {{-0.5, -0.5}, {-0.5, 0.5}, {0.5, 0.5}, {0.5, -0.5}};
  // Sloped runway: alt_m is the centre; the far end is higher when slope > 0 (uphill in this landing direction).
  // (The Python reference drew the whole outline at the centre altitude; with slope 0 both agree exactly.)
  const double rise_per_m = std::tan(rad(slope));
  std::array<Corner, 4> out{};
  for (int i = 0; i < 4; ++i) {
    const double a = kAS[i][0], s = kAS[i][1];
    // numpy: u * a * length + r * s * width, evaluated left to right per component
    const double e = u.x * a * length + r.x * s * width;
    const double n = u.y * a * length + r.y * s * width;
    const Vec2 ll = offset_latlon(lat, lon, e, n);
    out[i] = {ll.x, ll.y, alt_m + a * length * rise_per_m};
  }
  return out;
}

std::array<Runway::Corner, 4> Runway::patch(double a0, double a1, double r0, double r1) const {
  const double h = rad(hdg);
  const Vec2 u{std::sin(h), std::cos(h)};   // along, east/north
  const Vec2 r{std::cos(h), -std::sin(h)};  // right
  const double rise_per_m = std::tan(rad(slope));
  const double as[4] = {a0, a0, a1, a1}, rs[4] = {r0, r1, r1, r0};
  std::array<Corner, 4> out{};
  for (int i = 0; i < 4; ++i) {
    const double a = as[i] - 0.5 * length;  // from the centre
    const Vec2 ll = offset_latlon(lat, lon, u.x * a + r.x * rs[i], u.y * a + r.y * rs[i]);
    out[i] = {ll.x, ll.y, alt_m + a * rise_per_m};
  }
  return out;
}

TouchdownMarks touchdown_marks(const Runway& rw, bool faa) {
  TouchdownMarks m;
  const double L = rw.length, half_w = 0.5 * rw.width;
  if (L < 300 || rw.width < 10) return m;
  constexpr double ft = 0.3048;
  m.zone_end_m = std::min(faa ? 3000 * ft : 900.0, L / 3.0);
  // stripes sit symmetric about the centreline: inner edges `gap` apart, never past the runway edge
  auto pair = [&](double a0, double len, double gap, double w, std::vector<PatchM>& out) {
    gap = std::min(gap, 2 * half_w - 2 * w - 2);
    if (gap < 2) return;
    out.push_back({a0, a0 + len, -0.5 * gap - w, -0.5 * gap});
    out.push_back({a0, a0 + len, 0.5 * gap, 0.5 * gap + w});
  };
  if (faa) {
    // aiming point: 150 ft x 30 ft, 1,020 ft in, inner edges 72 ft apart on a 150 ft runway (less when narrower)
    const double gap = std::min(72 * ft, 0.48 * rw.width);
    if (L >= 4200 * ft) pair(1020 * ft, 150 * ft, gap, 30 * ft, m.aim);
    else pair(std::min(1020 * ft, L / 4), 100 * ft, gap, 20 * ft, m.aim);
    // touchdown zone bars: 75 ft x 6 ft, 5 ft apart, every 500 ft (none at 1,000 ft: the aiming point)
    const int groups[] = {3, 0, 2, 2, 1, 1};
    for (int k = 0; k < 6; ++k) {
      const double a = (k + 1) * 500 * ft;
      if (!groups[k] || a + 75 * ft > m.zone_end_m + 1) continue;
      for (int b = 0; b < groups[k]; ++b) {
        const double inner = 0.5 * gap + b * 11 * ft;  // 6 ft bar + 5 ft space
        if (inner + 6 * ft > half_w - 1) break;
        m.bars.push_back({a, a + 75 * ft, -inner - 6 * ft, -inner});
        m.bars.push_back({a, a + 75 * ft, inner, inner + 6 * ft});
      }
    }
    return m;
  }
  // ICAO Annex 14, Table 5-1 (aiming point) and 5.2.6 (touchdown zone)
  double aim_at, len, w, gap;
  if (L < 800) aim_at = 150, len = 30, w = 4, gap = 6;
  else if (L < 1200) aim_at = 250, len = 30, w = 6, gap = 9;
  else if (L < 2400) aim_at = 300, len = 45, w = 6, gap = 18;
  else aim_at = 400, len = 50, w = 8, gap = 20;
  pair(aim_at, len, gap, w, m.aim);
  const int npairs = L < 900 ? 1 : L < 1200 ? 2 : L < 1500 ? 3 : L < 2400 ? 4 : 6;
  const int stripes[] = {3, 3, 2, 2, 1, 1};  // pattern A: three, then two, then one stripe each side
  int placed = 0;
  for (int k = 1; placed < npairs && k <= 8; ++k) {
    const double a = 150.0 * k;
    if (std::abs(a - aim_at) < 50 + len) continue;  // no pair at (or within 50 m of) the aiming point
    if (a + 22.5 > L / 2) break;
    const int n = stripes[std::min(placed, 5)];
    for (int b = 0; b < n; ++b) {
      const double inner = 0.5 * gap + b * 4.5;  // 3 m stripes, 1.5 m apart
      if (inner + 3 > half_w - 1) break;
      m.bars.push_back({a, a + 22.5, -inner - 3, -inner});
      m.bars.push_back({a, a + 22.5, inner, inner + 3});
    }
    ++placed;
  }
  return m;
}

std::string runway_name(int num, int des) {
  static const char* kDesig[] = {"", "L", "R", "C", "W", "A", "B"};
  char buf[32];
  if (num >= 1 && num <= 36) {
    std::snprintf(buf, sizeof buf, "%02d%s", num, (des >= 0 && des <= 6) ? kDesig[des] : "");
  } else {
    std::snprintf(buf, sizeof buf, "#%d", num);
  }
  return buf;
}

std::vector<Runway> ends_from_record(const std::string& icao, const RunwayRecord& r) {
  const double hdg = r.heading, length = r.length, width = r.width;
  if (!(r.lat >= -90 && r.lat <= 90 && r.lon >= -180 && r.lon <= 180 && hdg >= 0 && hdg <= 360.5 && length > 100 &&
        length < 7000 && width > 5 && width < 150))
    return {};
  const double slope = r.true_slope;
  const double s = (std::isfinite(slope) && std::abs(slope) < 20) ? kSlopeSign * slope : 0.0;
  return {make_runway(icao, r.lat, r.lon, r.alt_m, hdg, length, width,
                      runway_name(r.primary_number, r.primary_designator), s),
          make_runway(icao, r.lat, r.lon, r.alt_m, hdg + 180, length, width,
                      runway_name(r.secondary_number, r.secondary_designator), -s)};
}

const Runway* pick_runway(const std::vector<const Runway*>& ends, double lat, double lon, double trk,
                          const Runway* current, double max_dist) {
  auto score = [&](const Runway& rw, double& out) {
    const Vec2 en = enu(rw.lat, rw.lon, lat, lon);  // aircraft relative to the runway centre
    const double h = rad(rw.hdg);
    const double along = en.x * std::sin(h) + en.y * std::cos(h);  // negative = before the centre
    const double cross_ = en.x * std::cos(h) - en.y * std::sin(h);
    const double dang = std::abs(pymod(trk - rw.hdg + 180, 360.0) - 180);
    if (dang > 30 || along > rw.length / 2 || -along - rw.length / 2 > max_dist || std::abs(cross_) > 3000)
      return false;
    out = std::abs(cross_) + 60 * dang;
    return true;
  };
  double sc = 0, cur_sc = 0;
  const bool keep = current && score(*current, cur_sc);
  const Runway* best = nullptr;
  double best_sc = 0;
  for (const Runway* rw : ends) {
    if (rw && score(*rw, sc) && (!best || sc < best_sc)) {
      best = rw;
      best_sc = sc;
    }
  }
  // The current one stays unless another lines up clearly better. (It used to stay as long as it was valid at all, as
  // in the reference: picked while intercepting from the side, 10L at ZULS kept the outline although the aircraft
  // then lined up with the parallel 10R, user 11/22.) The margin keeps the outline from hopping between two that
  // score alike: 120 m of offset, or 2 deg of heading.
  constexpr double kSwitch = 120.0;
  if (keep && (!best || best == current || best_sc > cur_sc - kSwitch)) return current;
  return best;
}

std::vector<Vec3> world_points_cam(const std::array<Runway::Corner, 4>& pts, double lat0, double lon0,
                                   const Vec3& cam_pos, const Mat3& R) {
  std::vector<Vec3> out;
  out.reserve(pts.size());
  for (const auto& p : pts) {
    const Vec2 en = enu(lat0, lon0, p.lat, p.lon);
    const double up = p.alt_m - (en.x * en.x + en.y * en.y) / (2 * kEarthRadius);
    out.push_back(R.tmul(Vec3{en.x, en.y, up} - cam_pos));
  }
  return out;
}

std::vector<Vec3> clip_near(const std::vector<Vec3>& poly, double near_z) {
  std::vector<Vec3> out;
  const size_t n = poly.size();
  for (size_t i = 0; i < n; ++i) {
    const Vec3& a = poly[i];
    const Vec3& b = poly[(i + 1) % n];
    const bool ina = a.z >= near_z, inb = b.z >= near_z;
    if (ina) out.push_back(a);
    if (ina != inb) {
      const double t = (near_z - a.z) / (b.z - a.z);
      out.push_back(a + t * (b - a));
    }
  }
  return out;
}

std::vector<Polyline> clip_segments_outside(const Polyline& polyline, const Polyline& P) {
  if (P.size() < 3 || polyline.size() < 2) return {polyline};
  double lx0 = polyline[0].x, lx1 = lx0, ly0 = polyline[0].y, ly1 = ly0;
  for (const Vec2& q : polyline) {
    lx0 = std::min(lx0, q.x), lx1 = std::max(lx1, q.x);
    ly0 = std::min(ly0, q.y), ly1 = std::max(ly1, q.y);
  }
  double px0 = P[0].x, px1 = px0, py0 = P[0].y, py1 = py0;
  for (const Vec2& q : P) {
    px0 = std::min(px0, q.x), px1 = std::max(px1, q.x);
    py0 = std::min(py0, q.y), py1 = std::max(py1, q.y);
  }
  if (lx1 < px0 || lx0 > px1 || ly1 < py0 || ly0 > py1) return {polyline};  // bounding boxes do not touch

  const size_t n = P.size();
  double area = 0;  // Python sum() starts at int 0 and adds left to right
  for (size_t i = 0; i < n; ++i) area += P[i].x * P[(i + 1) % n].y - P[(i + 1) % n].x * P[i].y;
  const double sign = area > 0 ? 1.0 : -1.0;

  std::vector<Polyline> pieces;
  Polyline cur;
  for (size_t k = 0; k + 1 < polyline.size(); ++k) {
    const Vec2 a = polyline[k], b = polyline[k + 1];
    const Vec2 d = b - a;
    double t0 = 0.0, t1 = 1.0;
    bool inside_possible = true;
    for (size_t i = 0; i < n; ++i) {
      const Vec2 p = P[i], q = P[(i + 1) % n];
      const Vec2 e = q - p;
      const Vec2 nrm{sign * -e.y, sign * e.x};  // inward normal
      const Vec2 ap = a - p;
      const double num = nrm.x * ap.x + nrm.y * ap.y;
      const double den = nrm.x * d.x + nrm.y * d.y;
      if (std::abs(den) < 1e-12) {
        if (num < 0) {
          inside_possible = false;
          break;
        }
        continue;
      }
      const double t = -num / den;
      if (den > 0) t0 = std::max(t0, t);
      else t1 = std::min(t1, t);
      if (t0 > t1) {
        inside_possible = false;
        break;
      }
    }
    if (!inside_possible || t1 <= t0) {
      if (cur.empty()) cur.push_back(a);
      cur.push_back(b);
      continue;
    }
    if (t0 > 0) {
      if (cur.empty()) cur.push_back(a);
      cur.push_back(a + t0 * d);
    }
    if (cur.size() > 1) pieces.push_back(cur);
    cur.clear();
    if (t1 < 1) cur = {a + t1 * d, b};
  }
  if (cur.size() > 1) pieces.push_back(cur);
  return pieces;
}

}  // namespace birdie
