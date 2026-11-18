// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "core/border.h"

#include <algorithm>
#include <cmath>

namespace birdie {

Seat seat_of(double eye_lateral_m) { return eye_lateral_m < 0 ? Seat::kLeft : Seat::kRight; }
const char* seat_key(Seat s) { return s == Seat::kLeft ? "L" : "R"; }

std::vector<Vec2> default_border() {
  return {{-kCanvasHalfWDeg, kCanvasTopDeg},
          {kCanvasHalfWDeg, kCanvasTopDeg},
          {kCanvasHalfWDeg, kCanvasBottomDeg},
          {-kCanvasHalfWDeg, kCanvasBottomDeg}};
}

Settings::BorderPoly mirrored(const Settings::BorderPoly& b) {
  Settings::BorderPoly m = b;
  m.pts.assign(b.pts.rbegin(), b.pts.rend());
  for (auto& p : m.pts) p.x = -p.x;
  m.eye.x = -m.eye.x;
  return m;
}

std::optional<Settings::BorderPoly> border_for(const Settings::BorderSeats& seats, Seat seat, std::string* how) {
  const auto& own = seat == Seat::kLeft ? seats.left : seats.right;
  const auto& other = seat == Seat::kLeft ? seats.right : seats.left;
  if (own && own->pts.size() >= 3) {
    if (how) *how = "drawn";
    return own;
  }
  if (other && other->pts.size() >= 3) {
    if (how) *how = "mirrored";
    return mirrored(*other);
  }
  if (how) *how = "none";
  return std::nullopt;
}

bool sanitize_border(std::vector<Vec2>& pts) {
  std::vector<Vec2> out;
  for (auto p : pts) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
    out.push_back({std::clamp(p.x, -kBorderMaxDeg, kBorderMaxDeg), std::clamp(p.y, -kBorderMaxDeg, kBorderMaxDeg)});
  }
  pts = std::move(out);
  return pts.size() >= 3 && pts.size() <= static_cast<size_t>(kBorderMaxPoints);
}

bool inside_polygon(const std::vector<Vec2>& poly, Vec2 p) {
  bool in = false;  // even-odd ray casting
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const Vec2 a = poly[i], b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
  }
  return in;
}

double distance_to_edges(const std::vector<Vec2>& poly, Vec2 p) {
  double best = 1e300;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const Vec2 a = poly[j], d = poly[i] - poly[j];
    const double l2 = d.x * d.x + d.y * d.y;
    const double t = l2 > 0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2, 0.0, 1.0) : 0.0;
    best = std::min(best, std::hypot(p.x - (a.x + t * d.x), p.y - (a.y + t * d.y)));
  }
  return best;
}

bool all_inside(const std::vector<Vec2>& poly, const std::vector<Vec2>& pts, double margin) {
  if (poly.size() < 3) return true;
  for (const auto& p : pts)
    if (!inside_polygon(poly, p) || distance_to_edges(poly, p) < margin) return false;
  return true;
}

std::optional<double> fit_scale(const std::vector<Vec2>& poly, const std::vector<Vec2>& pts, double margin, double lo,
                                double hi) {
  auto fits = [&](double k) {
    std::vector<Vec2> s;
    s.reserve(pts.size());
    for (const auto& p : pts) s.push_back(k * p);
    return all_inside(poly, s, margin);
  };
  // whole hundredths, so the panel shows exactly what is set
  const int a = static_cast<int>(std::ceil(lo * 100 - 1e-9)), b = static_cast<int>(std::floor(hi * 100 + 1e-9));
  if (b < a || !fits(a / 100.0)) return std::nullopt;
  if (fits(b / 100.0)) return b / 100.0;
  int good = a, bad = b;
  while (bad - good > 1) {  // the instruments shrink towards the boresight: fitting is monotonic for a sane border
    const int mid = (good + bad) / 2;
    (fits(mid / 100.0) ? good : bad) = mid;
  }
  return good / 100.0;
}

}  // namespace birdie
