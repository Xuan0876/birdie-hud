// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "hud/anim.h"

#include <algorithm>
#include <cmath>

namespace birdie::hud {
namespace {

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
double smooth(double x) {  // smoothstep
  x = clamp01(x);
  return x * x * (3 - 2 * x);
}
double out_cubic(double x) {
  x = clamp01(x);
  return 1 - (1 - x) * (1 - x) * (1 - x);
}
double out_back(double x) {  // a little past the mark, then back
  x = clamp01(x);
  const double c1 = 1.4, c3 = c1 + 1;
  return 1 + c3 * std::pow(x - 1, 3) + c1 * std::pow(x - 1, 2);
}
double span(double p, double a, double b) { return clamp01((p - a) / (b - a)); }
// deterministic noise in 0..1 for the boot's flicker (changes 30 times a second)
double noise(double t) {
  const double v = std::sin(std::floor(t * 30.0) * 12.9898) * 43758.5453;
  return v - std::floor(v);
}

struct Box {
  double x0, y0, x1, y1;
};
Box bounds(const Scene& s) {
  Box b{1e300, 1e300, -1e300, -1e300};
  if (s.clip)
    for (const auto& q : *s.clip) b = {std::min(b.x0, q.x), std::min(b.y0, q.y), std::max(b.x1, q.x), std::max(b.y1, q.y)};
  if (b.x0 > b.x1) b = {0, 0, 2000, 1200};
  return b;
}

// a little command writer for the effects drawn over the HUD
struct Fx {
  CommandList& out;
  void style(const double* rgb, double a, double glow_a, double glow_w) {
    out.cmds.insert(out.cmds.end(), {10.0, rgb[0], rgb[1], rgb[2], a, rgb[0], rgb[1], rgb[2], glow_a, 0.0, 0.0, 0.0,
                                     0.5 * a, glow_w});
  }
  void line(const Polyline& p, double w, bool closed = false) {
    if (p.size() < 2) return;
    out.cmds.insert(out.cmds.end(), {1.0, w, 0.0, closed ? 1.0 : 0.0, static_cast<double>(p.size())});
    for (const auto& q : p) out.cmds.insert(out.cmds.end(), {q.x, q.y});
  }
  void text(double x, double y, double px, const std::wstring& s) {
    out.cmds.insert(out.cmds.end(), {3.0, x, y, px, 1.0, 0.0, 0.0, static_cast<double>(out.text.size()),
                                     static_cast<double>(s.size())});
    out.text += s;
  }
  void clip(const Polyline& p) {
    out.cmds.insert(out.cmds.end(), {4.0, static_cast<double>(p.size())});
    for (const auto& q : p) out.cmds.insert(out.cmds.end(), {q.x, q.y});
  }
  void pop() { out.cmds.push_back(5.0); }
  void flush() { out.cmds.push_back(6.0); }
};

Polyline rect(double x0, double y0, double x1, double y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }
Polyline circle(Vec2 c, double r, int n = 72) {
  Polyline p;
  for (int i = 0; i < n; ++i) {
    const double a = 2 * kPi * i / n;
    p.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
  }
  return p;
}

}  // namespace

AnimKind anim_kind(const std::string& n) {
  if (n == "fade") return AnimKind::Fade;
  if (n == "iris") return AnimKind::Iris;
  if (n == "boot") return AnimKind::Boot;
  if (n == "scan") return AnimKind::Scan;
  if (n == "assemble") return AnimKind::Assemble;
  return AnimKind::None;
}

double anim_duration(AnimKind k, bool opening, double time_scale) {
  double d = 0;
  switch (k) {
    case AnimKind::None: return 0.0;
    case AnimKind::Fade: d = opening ? 0.45 : 0.35; break;
    case AnimKind::Iris: d = opening ? 0.75 : 0.5; break;
    case AnimKind::Boot: d = opening ? 1.4 : 0.6; break;
    case AnimKind::Scan: d = opening ? 0.9 : 0.6; break;
    case AnimKind::Assemble: d = opening ? 1.2 : 0.7; break;
  }
  return d * std::clamp(time_scale, 0.3, 3.0);
}

Settings anim_settings(const Settings& set, AnimKind k, double p) {
  if (k != AnimKind::Assemble || p >= 1.0) return set;
  Settings s = set;
  // each part on its own beat: the tapes first, then the bank scale and heading, the ladder unfolds, the bird last
  const double tapes = out_back(span(p, 0.0, 0.6)), top = out_back(span(p, 0.15, 0.75)), bird = out_back(span(p, 0.35, 1.0));
  const double ladder = out_cubic(span(p, 0.25, 0.9));
  s.spd_dx -= (1 - tapes) * 9.0;
  s.wind_dx -= (1 - tapes) * 9.0;
  s.alt_dx += (1 - tapes) * 9.0;
  s.roll_dy -= (1 - top) * 7.0;
  s.hdg_dy += (1 - top) * 7.0;
  s.bird_size = std::max(0.05, s.bird_size * (1 + (1 - bird) * 2.5));
  s.ladder_w = std::max(0.05, s.ladder_w * ladder);
  return s;
}

CommandList animated_commands(const Scene& scene, double opacity, AnimKind k, double p, double t) {
  CommandList out;
  p = clamp01(p);
  if (p <= 0.0) return out;
  if (p >= 1.0 || k == AnimKind::None) {
    append_commands(out, scene, opacity);
    return out;
  }
  Fx fx{out};
  const Box b = bounds(scene);
  const Vec2 c = scene.bore.value_or(Vec2{0.5 * (b.x0 + b.x1), 0.5 * (b.y0 + b.y1)});
  const double W = b.x1 - b.x0, H = b.y1 - b.y0;
  switch (k) {
    case AnimKind::Fade:
      append_commands(out, scene, opacity * smooth(p));
      break;
    case AnimKind::Assemble:  // the parts move through anim_settings(); here they fade in early
      append_commands(out, scene, opacity * smooth(span(p, 0.0, 0.45)));
      break;
    case AnimKind::Iris: {
      double R = 0;
      for (double x : {b.x0, b.x1})
        for (double y : {b.y0, b.y1}) R = std::max(R, std::hypot(x - c.x, y - c.y));
      const double e = out_cubic(p), r = std::max(1.0, e * R);
      fx.clip(circle(c, r));
      append_commands(out, scene, opacity * smooth(span(p, 0.0, 0.5)));
      fx.pop();
      const double rim = opacity * std::sin(kPi * p);  // the rim is brightest half-way, gone at both ends
      if (scene.clip) fx.clip(*scene.clip);  // the rim only inside the HUD's field
      fx.style(scene.rgb, 0.85 * rim, 0.35 * rim, 10.0);
      fx.line(circle(c, r), 2.2, true);
      fx.flush();
      if (scene.clip) fx.pop();
      break;
    }
    case AnimKind::Scan: {
      const double e = smooth(p), y = b.y0 + e * (H + 24) - 12;
      fx.clip(rect(b.x0 - 20, b.y0 - 20, b.x1 + 20, y));
      append_commands(out, scene, opacity);
      fx.pop();
      const double a = opacity * std::min(1.0, 4 * std::min(p, 1 - p) + 0.15);
      fx.style(scene.rgb, a, 0.4 * a, 14.0);
      fx.line({{b.x0, y}, {b.x1, y}}, 2.5);
      fx.flush();
      fx.style(scene.rgb, 0.25 * a, 0.12 * a, 30.0);  // the afterglow just above the line
      fx.line({{b.x0, y - 10}, {b.x1, y - 10}}, 1.0);
      fx.flush();
      break;
    }
    case AnimKind::Boot: {
      // 0 .. 0.2  a line grows out of the boresight; 0.2 .. 0.55 the picture rolls open around it;
      // 0.55 .. 1 it settles, flickering less and less, with a short self-test line under the bird's place
      const double flick = 0.55 + 0.45 * noise(t);
      if (p < 0.2) {
        const double e = out_cubic(p / 0.2), half = 0.5 * W * e;
        fx.style(scene.rgb, opacity * (0.6 + 0.4 * flick), 0.5 * opacity, 12.0);
        fx.line({{c.x - half, c.y}, {c.x + half, c.y}}, 2.0);
        fx.flush();
        break;
      }
      const double open = out_cubic(span(p, 0.2, 0.55));
      const double up = (c.y - b.y0 + 20) * open + 1.5, down = (b.y1 - c.y + 20) * open + 1.5;
      const double settle = span(p, 0.55, 1.0);
      const double a = opacity * (settle > 0 ? (1 - (1 - settle) * 0.35 * (1 - flick)) : (0.5 + 0.5 * flick));
      fx.clip(rect(b.x0 - 20, c.y - up, b.x1 + 20, c.y + down));
      append_commands(out, scene, a);
      fx.pop();
      if (open < 1) {  // the bright edges of the opening picture
        const double ea = opacity * (1 - open);
        fx.style(scene.rgb, ea, 0.5 * ea, 12.0);
        fx.line({{b.x0, c.y - up}, {b.x1, c.y - up}}, 2.0);
        fx.line({{b.x0, c.y + down}, {b.x1, c.y + down}}, 2.0);
        fx.flush();
      }
      const double ta = opacity * smooth(span(p, 0.3, 0.45)) * (1 - smooth(span(p, 0.8, 1.0)));
      if (ta > 0.01) {
        fx.style(scene.rgb, ta, 0.0, 1.0);
        const int dots = static_cast<int>(span(p, 0.35, 0.7) * 6.0);
        std::wstring s = L"HUD TEST ";
        for (int i = 0; i < 6; ++i) s += i < dots ? L"·" : L" ";
        s += dots >= 6 ? L" OK" : L"   ";
        fx.text(c.x, c.y + 0.22 * H, std::max(14.0, 0.022 * H), s);
        fx.flush();
      }
      break;
    }
    case AnimKind::None: break;
  }
  return out;
}

}  // namespace birdie::hud
