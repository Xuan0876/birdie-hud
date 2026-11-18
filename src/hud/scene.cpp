// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "hud/scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "core/border.h"
#include "core/geo.h"

namespace birdie::hud {
namespace {

// ---------------------------------------------------------------- canvas constants (hud.py)
constexpr double CW = 1280.0, CH = 1024.0;
constexpr double CX = 640.0;
constexpr double BORE_Y = 329.143;  // aircraft reference (longitudinal axis), 5 deg above the canvas centre
constexpr double ONE_DEG = 36.5714;
const double F_CANVAS = ONE_DEG * 180.0 / kPi;  // canvas px per radian at the centre
constexpr double SPD_LINE_X = 140.882, TAPE_TOP = 328.865, TAPE_BOT = 692.13, TAPE_MID = 510.5;
constexpr double SPD_PX_PER_KT = 4.25;
constexpr double ALT_LINE_X = 1113.112, ALT_PX_PER_FT = 0.32;
constexpr double ALT_BOX[4] = {1057.3, 480.1, 1150.8, 540.9};
constexpr double ROLL_C[2] = {640.0, 329.143};
constexpr double TEXT_GAIN = 1.2;
const wchar_t* const kMinus = L"−";

// ---------------------------------------------------------------- Python-compatible formatting
std::wstring fmt(const char* f, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return std::wstring(buf, buf + std::char_traits<char>::length(buf));
}
// Python round(x) -> int (ties to even; the default FP rounding mode)
long long pyround(double x) { return static_cast<long long>(std::nearbyint(x)); }
// Python round(x, 1): correctly rounded decimal
double round1(double x) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.1f", x);
  return std::strtod(buf, nullptr);
}
// Python int(x) for floats: truncation towards zero
long long pyint(double x) { return static_cast<long long>(std::trunc(x)); }

Vec3 dir_deg(double az_deg, double el_deg) {
  const double az = rad(az_deg), el = rad(el_deg);
  return {std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)};
}

// ---------------------------------------------------------------- camera + HUD plane for one frame
struct Frame {
  SimState p;
  Mat3 R, R_bc;  // camera -> ENU, body -> camera
  double f, W, H, ox, oy, k;

  OptVec2 pix(Vec3 d) const {
    const Vec3 r = R.tmul(d);
    if (r.z <= 1e-3) return std::nullopt;
    return Vec2{W / 2 + f * r.x / r.z - ox, H / 2 - f * r.y / r.z - oy};
  }
  OptVec2 canvas_pix(double u, double v) const {
    const Vec3 b{(u - CX) * k, (BORE_Y - v) * k, F_CANVAS};
    const Vec3 c = R_bc * b;
    if (c.z <= 1e-3) return std::nullopt;
    return Vec2{W / 2 + f * c.x / c.z - ox, H / 2 - f * c.y / c.z - oy};
  }
  double screen_per_canvas() const { return f / F_CANVAS * k; }
};

// The camera for helpers that need no runway (HUD border editing, fit check).
Frame frame_for(const Model& model, const SimState& s, double W, double H, double ox, double oy, double k) {
  const Geometry geo = model.geometry(s, W, H, nullptr);
  return Frame{geo.p, geo.R, camera_to_body(geo.p.cp_yaw, geo.p.cp_pitch, geo.p.cp_roll).transposed(), geo.f, W, H,
               ox, oy, k};
}

// HUD-plane degrees (core/border.h) -> camera-frame direction. Same plane as canvas_pix at size 1.
Vec3 border_cam(const Frame& fr, Vec2 p) { return fr.R_bc * Vec3{rad(p.x), rad(p.y), 1.0}; }
constexpr double kBorderNear = 0.02;  // near plane for the border polygon (camera z of a unit-z ray)

std::optional<Polyline> project_border(const Frame& fr, const std::vector<Vec2>& pts) {
  if (pts.size() < 3) return std::nullopt;
  std::vector<Vec3> c, in;
  for (const auto& p : pts) c.push_back(border_cam(fr, p));
  for (size_t i = 0; i < c.size(); ++i) {  // Sutherland-Hodgman against z >= kBorderNear
    const Vec3 a = c[i], b = c[(i + 1) % c.size()];
    const bool ain = a.z >= kBorderNear, bin = b.z >= kBorderNear;
    if (ain) in.push_back(a);
    if (ain != bin) in.push_back(a + ((kBorderNear - a.z) / (b.z - a.z)) * (b - a));
  }
  if (in.size() < 3) return std::nullopt;
  Polyline out;
  for (const auto& q : in) out.push_back({fr.W / 2 + fr.f * q.x / q.z - fr.ox, fr.H / 2 - fr.f * q.y / q.z - fr.oy});
  return out;
}

struct Conformal {
  Polyline horizon;  // empty = none
  std::optional<std::vector<Polyline>> horizon_pieces;
  std::vector<std::pair<Vec2, Vec2>> hdg_ticks;
  struct Label {
    Vec2 p;
    double ang;
    std::wstring txt;
  };
  std::vector<Label> hdg_labels;
  struct RungLabel {
    Vec2 p;
    double ang;
    int side;
    std::wstring txt;
  };
  struct Rung {
    double e;
    std::vector<Polyline> segs;
    std::vector<RungLabel> labels;
  };
  std::vector<Rung> rungs;
  std::vector<Polyline> minor;
};

Polyline keep(const std::vector<OptVec2>& pts) {
  Polyline out;
  for (const auto& q : pts)
    if (q) out.push_back(*q);
  return out;
}

double angle_deg(Vec2 l, Vec2 r) { return deg(std::atan2(r.y - l.y, r.x - l.x)); }

Conformal conformal(const Frame& fr, const Settings& set, double hdg_true, double hdg_mag) {
  Conformal out;
  if (set.show_horizon) {
    std::vector<OptVec2> pts;
    for (double a : linspace(-30, 30, 61)) pts.push_back(fr.pix(dir_deg(hdg_true + a, 0.0)));
    out.horizon = keep(pts);
  }
  if (set.show_hdg && set.sty_hdg != "arc" && set.sty_hdg != "tape") {  // the arc and the tape: heading_fixed()
    const double magvar = wrap180(hdg_true - hdg_mag);
    const long long base = static_cast<long long>(std::floor((hdg_mag - 20) / 5.0)) * 5;
    for (long long h = base; h < pyint(hdg_mag + 21); h += 5) {
      const double az = h + magvar;
      const bool ten = pymod(h, 10LL) == 0;
      const auto a = fr.pix(dir_deg(az, 0.0));
      const auto b = fr.pix(dir_deg(az, -(ten ? 14.0 : 8.0) / ONE_DEG));
      if (a && b) out.hdg_ticks.push_back({*a, *b});
      if (ten) {
        const auto c = fr.pix(dir_deg(az, -30.0 / ONE_DEG));
        const auto l = fr.pix(dir_deg(az - 0.5, 0.0)), r = fr.pix(dir_deg(az + 0.5, 0.0));
        if (c && l && r) {
          const long long hh = pymod(h, 360LL);
          out.hdg_labels.push_back({*c, angle_deg(*l, *r), hh == 0 ? L"0" : fmt("%lld", pyfloordiv(hh, 10))});
        }
      }
    }
  }
  const double step = set.ladder_step_deg;
  if (step > 0) {
    // rung shapes: Airbus (3.33 .. 5.27 deg each side, 4 dashes below), Boeing (wider gap, longer rungs, 3 dashes),
    // fighter (rungs bent towards the horizon by half the pitch angle), minimal (short, plain, no numbers)
    const std::string& ls = set.sty_ladder;
    const bool boeing = ls == "boeing", fighter = ls == "fighter", minimal = ls == "minimal";
    double inner = (CX - 518.26) / ONE_DEG, outer = (CX - 447.16) / ONE_DEG;
    if (boeing) inner = 2.6, outer = 6.4;
    else if (fighter) inner = 2.2, outer = 6.0;
    else if (minimal) inner = 1.6, outer = 3.0;
    if (set.ladder_w != 1.0) outer = inner + (outer - inner) * set.ladder_w;
    const double tick = (fighter ? 16.0 : 11.0) / ONE_DEG;
    const int dashes = boeing ? 3 : minimal ? 2 : 4;
    const double dash_len = (outer - inner) / (dashes * 1.64 - 0.64);  // the Airbus proportions: 12 on, 7.7 off
    const long long kmax = pyint(std::min(set.ladder_max_deg, 30.0) / step + 1e-6);
    for (long long k = -kmax; k <= kmax; ++k) {
      if (k == 0) continue;
      const double e = k * step;
      // fighter rungs lean: the outer end is closer to the horizon by tan(|e| / 2) of the rung's length
      auto el = [&](double a) { return fighter ? e - (e > 0 ? 1 : -1) * (a - inner) * std::tan(rad(std::abs(e) / 2)) : e; };
      std::vector<Polyline> segs;
      for (int side : {-1, 1}) {
        if (e > 0) {  // solid rung, end tick down towards the horizon
          std::vector<OptVec2> pts;
          for (double a : linspace(inner, outer, 6)) pts.push_back(fr.pix(dir_deg(hdg_true + side * a, el(a))));
          if (!minimal) pts.push_back(fr.pix(dir_deg(hdg_true + side * outer, el(outer) - tick)));
          segs.push_back(keep(pts));
        } else if (!boeing && !fighter && !minimal && set.ladder_w == 1.0) {  // dashed: 4 dashes of 12 px every 19.7 px, the last one with an end tick up
          for (int j = 0; j < 4; ++j) {
            const double a0 = inner + j * 19.7 / ONE_DEG;
            const double a1 = a0 + 12.0 / ONE_DEG;
            std::vector<OptVec2> pts{fr.pix(dir_deg(hdg_true + side * a0, e)),
                                     fr.pix(dir_deg(hdg_true + side * a1, e))};
            if (j == 3) pts.push_back(fr.pix(dir_deg(hdg_true + side * a1, e + tick)));
            segs.push_back(keep(pts));
          }
        } else {
          for (int j = 0; j < dashes; ++j) {
            const double a0 = inner + j * dash_len * 1.64, a1 = a0 + dash_len;
            std::vector<OptVec2> pts;
            for (double a : linspace(a0, a1, 3)) pts.push_back(fr.pix(dir_deg(hdg_true + side * a, el(a))));
            if (j == dashes - 1 && !minimal) pts.push_back(fr.pix(dir_deg(hdg_true + side * a1, el(a1) + tick)));
            segs.push_back(keep(pts));
          }
        }
      }
      std::vector<Conformal::RungLabel> labels;
      for (int side : {-1, 1}) {
        if (minimal) break;
        const double la = outer + 2.5 / ONE_DEG;
        const auto q = fr.pix(dir_deg(hdg_true + side * la, fighter ? el(la) : e));
        const auto l = fr.pix(dir_deg(hdg_true - 0.5, e)), r = fr.pix(dir_deg(hdg_true + 0.5, e));
        if (q && l && r) labels.push_back({*q, angle_deg(*l, *r), side, fmt("%.0f", e)});
      }
      Conformal::Rung rung{e, {}, std::move(labels)};
      for (auto& s : segs)
        if (s.size() > 1) rung.segs.push_back(std::move(s));
      out.rungs.push_back(std::move(rung));
    }
  }
  if (set.show_pitch_minor) {  // PFD-style short 2.5 deg marks, centred on the heading
    const double half = 12.0 / ONE_DEG;
    const double top = std::min(set.ladder_max_deg, 30.0);
    const long long jn = pyint(top / 2.5);
    for (long long j = -jn; j <= jn; ++j) {
      const double e = j * 2.5;
      if (j == 0 || (step > 0 && std::abs(e / step - std::nearbyint(e / step)) < 1e-6)) continue;
      const auto a = fr.pix(dir_deg(hdg_true - half, e)), b = fr.pix(dir_deg(hdg_true + half, e));
      if (a && b) out.minor.push_back({*a, *b});
    }
  }
  return out;
}

// ---------------------------------------------------------------- airframe-fixed symbols (canvas space)
// Crosswind mode (Birdie's own, after the Airbus HUD's crosswind declutter): the speed and altitude tapes get shorter
// and move outward, so the drifted bird and runway have the middle of the view; the VS needle is left out.
constexpr double kXwSquash = 0.55, kXwSpeedDx = -50.0, kXwAltDx = 60.0;

// Where each group of airframe-fixed symbols goes (the layout settings): canvas offset + scale about its anchor.
struct Groups {
  Xf spd, alt, vs, wind, roll, att, hdg;
};
Groups groups(const Settings& set) {
  const double D = ONE_DEG, sp = set.lay_spread - 1.0;
  const double side_s = (set.xwind_mode ? kXwSpeedDx : 0.0) - sp * (CX - SPD_LINE_X);
  const double side_a = (set.xwind_mode ? kXwAltDx : 0.0) + sp * (ALT_LINE_X - CX);
  Groups g;
  g.spd = {side_s + set.spd_dx * D, set.spd_dy * D, set.spd_size, SPD_LINE_X, TAPE_MID};
  g.alt = {side_a + set.alt_dx * D, set.alt_dy * D, set.alt_size, ALT_LINE_X, TAPE_MID};
  g.vs = {g.alt.dx + set.vs_dx * D, g.alt.dy + set.vs_dy * D, set.vs_size, 1185.0, TAPE_MID};
  // ground speed and wind sit near the canvas edge: they follow the crosswind move, not the spread
  g.wind = {(set.xwind_mode ? kXwSpeedDx : 0.0) + set.wind_dx * D, set.wind_dy * D, set.wind_size, 68.0, 300.0};
  g.roll = {0.0, set.roll_dy * D, set.roll_size, ROLL_C[0], ROLL_C[1]};
  g.att = {0.0, 0.0, set.att_size, CX, 332.0};
  g.hdg = {0.0, set.hdg_dy * D, 1.0, CX, BORE_Y};
  return g;
}

struct TapeSpan {
  double top, bot;
};
TapeSpan tape_span(const Settings& set, double len) {
  if (!set.xwind_mode && len == 1.0) return {TAPE_TOP, TAPE_BOT};
  const double k = len * (set.xwind_mode ? kXwSquash : 1.0);
  return {TAPE_MID - (TAPE_MID - TAPE_TOP) * k, TAPE_MID + (TAPE_BOT - TAPE_MID) * k};
}

// A readout box from x0 to x1 around yc (half height h), with a point of length tip on the right (side +1) or the
// left (-1) side, or none (0).
Polyline readout_box(double x0, double x1, double yc, double h, int side, double tip) {
  if (side > 0) return {{x0, yc - h}, {x1, yc - h}, {x1 + tip, yc}, {x1, yc + h}, {x0, yc + h}};
  if (side < 0) return {{x0, yc - h}, {x1, yc - h}, {x1, yc + h}, {x0, yc + h}, {x0 - tip, yc}};
  return {{x0, yc - h}, {x1, yc - h}, {x1, yc + h}, {x0, yc + h}};
}

void speed_tape(Batch& B, const SimState& s, const HudFilters& filt, const Settings& set) {
  if (!set.show_speed) return;
  const double ias = s.ias_kt;
  const double w = 3.0;
  const bool box = set.sty_speed == "box", boeing = set.sty_speed == "boeing";
  const auto [top, bot] = tape_span(set, set.spd_len);
  if (!box) {
    B.line(Polyline{{SPD_LINE_X, top}, {SPD_LINE_X, bot}}, w, true);
    B.line(Polyline{{68.1, top}, {161.0, top}}, w, true);
    B.line(Polyline{{68.1, bot}, {161.0, bot}}, w, true);
    const long long lo = static_cast<long long>(std::floor((ias - 45) / 10.0)) * 10;
    for (long long v = std::max(30LL, lo); v < pyint(ias + 46); v += 10) {
      const double y = TAPE_MID - (v - ias) * SPD_PX_PER_KT;
      if (top + 4 < y && y < bot - 4) {
        B.line(Polyline{{SPD_LINE_X - 12.0, y}, {SPD_LINE_X, y}}, w, true);
        const bool under_box = boeing && std::abs(y - TAPE_MID) < 32;
        if (v % 20 == 0 && top + 14 < y && y < bot - 6 && !under_box) B.text(94.0, y + 9.27, fmt("%lld", v), 26, kMid, true);
      }
    }
  }
  if (box || boeing) {  // the speed as a number in a box (pointing at the tape on the Boeing)
    const double x0 = box ? 50.0 : 56.0, x1 = box ? 136.0 : 126.0;
    B.line(readout_box(x0, x1, TAPE_MID, 21.0, boeing ? 1 : 0, 12.0), w, true, true);
    B.text(0.5 * (x0 + x1), TAPE_MID + 11.5, fmt("%lld", pyround(std::max(0.0, ias))), 32, kMid, true);
  } else {
    // fixed speed index: bar across the scale + pointer
    B.fill({{119.47, 509.5}, {119.47, 512.58}, {147.3, 512.58}, {160.57, 517.47}, {160.57, 504.62}, {147.3, 509.5}},
           true);
  }
  double tr = filt.trend10();
  if (!box && std::abs(tr) > 2.0) {
    const double lim = std::min(42.0, (TAPE_MID - top) / SPD_PX_PER_KT);  // 42 kt; less on the short tape
    tr = std::max(-lim, std::min(lim, tr));
    const double y1 = TAPE_MID - tr * SPD_PX_PER_KT;
    const int sg = tr > 0 ? 1 : -1;
    B.line(Polyline{{168.0, TAPE_MID}, {168.0, y1}}, w, true);
    B.line(Polyline{{161.0, y1 + sg * 10}, {168.0, y1}, {175.0, y1 + sg * 10}}, w, true);
  }
  const double mach = s.mach;
  if (mach >= 0.5)
    B.text(box ? 93.0 : 104.0, box ? TAPE_MID + 60.0 : 730.0 - (TAPE_BOT - bot),
           fmt(".%02lld", pymod(pyround(mach * 100), 100LL)), 28, kMid, true);
}

// Wind indicator as on the reference HUD: "DDD / SS" and an arrow showing where the wind blows, relative to the
// nose (up = straight ahead).
void wind(Batch& B, const SimState& s) {
  const double wd = s.wind_dir, wk = s.wind_kt;
  const double hdg_mag = s.hdg_mag;
  const double magvar = wrap180(s.hdg_true - hdg_mag);
  const double wd_mag = pymod(wd - magvar, 360.0);
  const double x0 = 273.0, y0 = 258.0;
  B.text(x0 + 25, y0, fmt("%03lld", pymod(pyround(wd_mag), 360LL)), 22, kEnd, true);
  B.text(x0 + 33, y0 - 1, L"/", 20, kMid, true);
  B.text(x0 + 42, y0, fmt("%lld", pyround(wk)), 22, kStart, true);
  if (wk > 2) {
    const double rot = rad(static_cast<double>(pymod(pyround(wd_mag) - pyround(hdg_mag) + 180, 360LL)));
    const double cx = x0 + 3, cy = y0 + 10 + 15;  // rotation centre = middle of the 30 px shaft
    const double cr = std::cos(rot), sr = std::sin(rot);
    auto r = [&](double px, double py) {
      const double dx = px, dy = py - 15;
      return Vec2{cx + dx * cr - dy * sr, cy + dx * sr + dy * cr};
    };
    B.line(Polyline{r(0, 30), r(0, 0)}, 3.0, true);
    B.line(Polyline{r(-6.5, 12), r(0, 0), r(6.5, 12)}, 3.0, true);
  }
}

// Ground speed and wind, top left (their own group: they move with the speed tape's side, not its offsets).
void ground_speed(Batch& B, const SimState& s, const Settings& set) {
  if (!set.show_speed || !set.show_gs) return;
  B.text(68.0, 300.0, fmt("GS %.0f", s.gs_kt), 24, kStart, true);
  wind(B, s);
}

struct Drum {
  Polyline rect;  // canvas
  struct Digit {
    double x, y;
    std::wstring txt;
  };
  std::vector<Digit> digits;
};

std::optional<Drum> altitude_tape(Batch& B, const SimState& s, const Settings& set) {
  const double w = 3.0;
  std::optional<Drum> drum;
  const double alt = s.ind_alt_ft;
  if (!set.show_alt) return drum;
  const bool box = set.sty_alt == "box", boeing = set.sty_alt == "boeing";
  const auto [top, bot] = tape_span(set, set.alt_len);
  if (!box) {
    B.line(Polyline{{1057.4, bot}, {1133.2, bot}}, w, true);
    B.line(Polyline{{ALT_LINE_X, top}, {ALT_LINE_X, bot}}, w, true);
    B.line(Polyline{{1057.4, top}, {1133.2, top}}, w, true);
    // the Airbus readout sits on the tape: no ticks behind it; the Boeing box is beside the tape
    const double by0 = boeing ? -1e9 : ALT_BOX[1], by1 = boeing ? -1e9 : ALT_BOX[3];
    const long long lo = static_cast<long long>(std::floor((alt - 650) / 100.0)) * 100;
    for (long long v = lo; v < pyint(alt + 651); v += 100) {
      const double y = TAPE_MID - (v - alt) * ALT_PX_PER_FT;
      if (!(top + 4 < y && y < bot - 4) || (by0 - 4 < y && y < by1 + 4)) continue;
      B.line(Polyline{{ALT_LINE_X - 8.56, y}, {ALT_LINE_X, y}}, w, true);
      if (pymod(v, 500LL) == 0 && top + 14 < y && y < bot - 6 && !(by0 - 12 < y && y < by1 + 18)) {
        const std::wstring txt = v >= 0 ? fmt("%lld", pyfloordiv(v, 100)) : fmt("-%lld", pyfloordiv(std::llabs(v), 100));
        B.text(1081.0, y + 8.5, txt, 26, kMid, true);
      }
    }
  }
  if (box || boeing) {  // the altitude as a number in a box (pointing at the tape on the Boeing), 20 ft steps
    const double x0 = box ? 1050.0 : 1124.0, x1 = box ? 1150.0 : 1206.0;
    B.line(readout_box(x0, x1, TAPE_MID, 21.0, boeing ? -1 : 0, 11.0), w, true, true);
    const long long v = pyround(alt / 20.0) * 20;
    B.text(0.5 * (x0 + x1), TAPE_MID + 11.5, (v < 0 ? kMinus : L"") + fmt("%lld", std::llabs(v)), 30, kMid, true);
  } else {
    // readout: hundreds and above in the left part, rolling tens drum on the right
    B.line(Polyline{{1113.1, 529.6},
                    {1057.3, 529.6},
                    {1057.3, 491.4},
                    {1113.1, 491.4},
                    {1113.1, 480.1},
                    {1150.8, 480.1},
                    {1150.8, 540.9},
                    {1113.1, 540.9},
                    {1113.1, 529.6}},
           w, true);
    const double a = std::abs(alt);
    B.text(1110.0, 522.0, (alt < 0 ? L"-" : L"") + fmt("%lld", static_cast<long long>(std::floor(a / 100))), 34, kEnd,
           true);
    const double tens = pymod(a, 100.0);
    const long long base = static_cast<long long>(std::floor(tens / 20)) * 20;
    const double frac = (tens - base) / 20.0;
    Drum d;
    d.rect = {{1114.6, 481.6}, {1149.3, 481.6}, {1149.3, 539.4}, {1114.6, 539.4}};
    for (int j : {-1, 0, 1, 2})
      d.digits.push_back({1132.0, 520.0 + (frac - j) * 27.0, fmt("%02lld", pymod(base + 20LL * j, 100LL))});
    drum = std::move(d);
  }
  if (s.baro_mb != 0)
    B.text(box ? 1100.0 : 1095.0, box ? TAPE_MID + 82.0 : 730.0 - (TAPE_BOT - bot), fmt("%.0f", s.baro_mb), 26, kMid,
           true);
  return drum;
}

// Vertical speed: the Airbus needle beside the altitude tape, or a number under the altitude readout.
void vertical_speed(Batch& B, const HudFilters& filt, const Settings& set) {
  if (!set.show_vs) return;
  const double vs = filt.vs;
  if (set.sty_vs == "digital") {  // under the altitude readout, whichever it is
    const long long v = pyround(vs / 10.0) * 10;
    const double x = set.sty_alt == "boeing" ? 1165.0 : set.sty_alt == "box" ? 1100.0 : 1104.0;
    B.text(x, TAPE_MID + 50.0, L"VS " + std::wstring(v < 0 ? kMinus : L"") + fmt("%lld", std::llabs(v)), 22, kMid, true);
    return;
  }
  if (set.xwind_mode) return;
  const double av = std::abs(vs);
  const int sg = vs >= 0 ? 1 : -1;
  double yo;
  if (av < 1000) yo = vs / 1000 * -136.1;
  else if (av < 2000) yo = (vs - sg * 1000) / 1000 * -50.5 - sg * 136.1;
  else if (av < 6000) yo = (vs - sg * 2000) / 4000 * -50.5 - sg * 186.6;
  else yo = sg * -237.0;
  B.fill({{1165, 508.5}, {1185, 508.5}, {1185, 512.5}, {1165, 512.5}}, true);
  B.line(Polyline{{1220.0, 510.5 + 0.26 * yo}, {1183.0, 510.5 + yo}}, 4.0, true);
  if (av >= 200) {
    const long long n = pyround(av / 100);
    const std::wstring txt = (vs < 0 ? L"-" : L"") + (n < 10 ? fmt("0%lld", n) : fmt("%lld", n));
    B.text(1197.0 + (vs > 0 ? 0 : -5), 518.0 + yo - sg * 14, txt, 22, kMid, true);
  }
}

void roll_scale(Batch& B, const SimState& s, const Settings& set) {
  if (!set.show_roll) return;
  const double w = 3.0;
  const double cx = ROLL_C[0], cy = ROLL_C[1];
  // roll index + sideslip, turning with the horizon (SimConnect bank: + = left wing down -> clockwise)
  const double ang = rad(s.bank_deg);
  const double ca = std::cos(ang), sa = std::sin(ang);
  auto rot = [&](double qx, double qy) {
    const double x = qx - cx, y = qy - cy;
    return Vec2{cx + x * ca - y * sa, cy + x * sa + y * ca};
  };
  const double slip = std::max(-30.0, std::min(30.0, s.beta_deg * 6.0));
  auto at = [&](double a_deg, double r) {  // a = bank angle on the scale (0 = top), r = radius
    const double t = rad(90.0 + a_deg);
    return Vec2{cx + r * std::cos(t), cy - r * std::sin(t)};
  };
  if (set.sty_roll == "boeing") {  // bare ticks (long at 30 and 60), a fixed zero index, the pointer under them
    for (double a : {-60.0, -45.0, -30.0, -20.0, -10.0, 10.0, 20.0, 30.0, 45.0, 60.0}) {
      const double len = (std::abs(a) == 30.0 || std::abs(a) == 60.0) ? 24.0 : 13.0;
      B.line(Polyline{at(a, 190.0), at(a, 190.0 + len)}, w, true);
    }
    B.line(Polyline{{640, 138.44}, {652.22, 118.24}, {627.79, 118.24}}, w, true, true);
    B.line(Polyline{rot(640, 144), rot(630, 162), rot(650, 162)}, w, true, true);
    B.line(Polyline{rot(630 + slip, 166), rot(650 + slip, 166), rot(650 + slip, 174), rot(630 + slip, 174)}, w, true,
           true);
    return;
  }
  if (set.sty_roll == "bottom") {  // a small bank scale low in the view (fighters); its pointer swings below
    constexpr double bx = 640.0, by = 760.0, R = 110.0;
    auto bat = [&](double a_deg, double r) {
      const double t = rad(-90.0 + a_deg);
      return Vec2{bx + r * std::cos(t), by - r * std::sin(t)};
    };
    auto brot = [&](double qx, double qy) {
      const double x = qx - bx, y = qy - by;
      return Vec2{bx + x * ca - y * sa, by + x * sa + y * ca};
    };
    Polyline arc;
    for (double a : linspace(-60.0, 60.0, 41)) arc.push_back(bat(a, R));
    B.line(arc, w, true);
    for (double a : {-60.0, -45.0, -30.0, -20.0, -10.0, 0.0, 10.0, 20.0, 30.0, 45.0, 60.0}) {
      const double len = (std::abs(a) == 30.0 || std::abs(a) == 60.0 || a == 0.0) ? 18.0 : 10.0;
      B.line(Polyline{bat(a, R), bat(a, R + len)}, w, true);
    }
    B.line(Polyline{brot(bx, by + R - 4), brot(bx - 9, by + R - 21), brot(bx + 9, by + R - 21)}, w, true, true);
    B.line(Polyline{brot(bx - 9 + slip, by + R - 26), brot(bx + 9 + slip, by + R - 26), brot(bx + 9 + slip, by + R - 32),
                    brot(bx - 9 + slip, by + R - 32)},
           w, true, true);
    return;
  }
  if (set.sty_roll == "airbus") {
    // the A320 HUD (SmartCockpit HUD notes): ticks at 10, 20, 30 and 45 deg on a short arc; the 60 deg ticks only
    // once the bank passes 40 deg. (The FlyByWire geometry kept as "a32nx" is the PFD's: an arc to 65 deg and the
    // 67 deg bank-limit marks, which the HUD does not draw.)
    Polyline arc;
    for (double a : linspace(-45.0, 45.0, 31)) arc.push_back(at(a, 190.0));
    B.line(arc, w, true);
    std::vector<double> ticks{-45.0, -30.0, -20.0, -10.0, 10.0, 20.0, 30.0, 45.0};
    if (std::abs(s.bank_deg) > 40.0) ticks.insert(ticks.end(), {-60.0, 60.0});
    for (double a : ticks) {
      const double len = std::abs(a) == 10.0 || std::abs(a) == 20.0 ? 13.0 : 24.0;
      B.line(Polyline{at(a, 190.0), at(a, 190.0 + len)}, w, true);
    }
    B.line(Polyline{{640, 138.44}, {652.22, 118.24}, {627.79, 118.24}}, w, true, true);
    B.line(Polyline{rot(640.18, 140), rot(630.18, 161.89), rot(650.18, 161.89)}, w, true, true);
    B.line(Polyline{rot(629 + slip, 164.85), rot(621 + slip, 181), rot(659 + slip, 181.07), rot(651 + slip, 164.85)}, w,
           true, true);
    return;
  }
  const double a0 = rad(155.16), a1 = rad(24.84);
  Polyline arc;
  for (double a : linspace(a0, a1, 40)) arc.push_back({cx + 190 * std::cos(a), cy - 190 * std::sin(a)});
  B.line(arc, w, true);
  static constexpr double kTicks[10][4] = {
      {735.2, 164, 749.3, 139.5}, {705.1, 150, 712.8, 129.3}, {673, 141.5, 677, 119.3}, {544.8, 164, 530.7, 139.5},
      {574.9, 150, 567.2, 129.3}, {607, 141.5, 603, 119.3},   {774.5, 194.5, 794.5, 174.5},
      {505.5, 194.5, 485.5, 174.5}, {813, 249, 832, 240},     {467, 249, 448, 240}};
  for (const auto& t : kTicks) B.line(Polyline{{t[0], t[1]}, {t[2], t[3]}}, w, true);
  B.line(Polyline{{640, 138.44}, {652.22, 118.24}, {627.79, 118.24}}, w, true, true);
  B.line(Polyline{rot(640.18, 140), rot(630.18, 161.89), rot(650.18, 161.89)}, w, true, true);
  B.line(Polyline{rot(629 + slip, 164.85), rot(621 + slip, 181), rot(659 + slip, 181.07), rot(651 + slip, 164.85)}, w,
         true, true);
}

// Fixed aircraft reference: wings + small square on the boresight (a fighter's "W" waterline with that bird).
void aircraft_ref(Batch& B, const Settings& set) {
  if (!set.show_att) return;
  const double w = 3.4;
  if (set.sty_fpv == "fighter") {
    B.line(Polyline{{596, 332}, {614, 332}, {626, 348}, {640, 334}, {654, 348}, {666, 332}, {684, 332}}, w, true);
    return;
  }
  B.line(Polyline{{625, 335}, {625, 329}, {595, 329}}, w, true);
  B.line(Polyline{{637, 332}, {643, 332}, {643, 326}, {637, 326}}, w, true, true);
  B.line(Polyline{{655, 335}, {655, 329}, {685, 329}}, w, true);
}

// Heading as an instrument (the marks on the horizon are conformal, see conformal()): a compass arc low in the view
// (Boeing) or a tape across the top (fighters).
void heading_fixed(Batch& B, const SimState& s, const Settings& set) {
  if (!set.show_hdg || (set.sty_hdg != "arc" && set.sty_hdg != "tape")) return;
  const double w = 3.0, hdg = s.hdg_mag;
  auto label = [](long long h) { return fmt("%lld", pyfloordiv(pymod(h, 360LL), 10LL)); };
  const long long now = pymod(pyround(hdg), 360LL);
  if (set.sty_hdg == "arc") {
    constexpr double cy = 1010.0, R = 230.0, half = 35.0;
    auto at = [&](double d, double r) { return Vec2{CX + r * std::sin(rad(d)), cy - r * std::cos(rad(d))}; };
    Polyline arc;
    for (double d : linspace(-half, half, 36)) arc.push_back(at(d, R));
    B.line(arc, w, true);
    const long long base = static_cast<long long>(std::floor((hdg - half) / 5.0)) * 5;
    for (long long h = base; h <= pyint(hdg + half) + 1; h += 5) {
      const double d = h - hdg;
      if (std::abs(d) > half) continue;
      const bool ten = pymod(h, 10LL) == 0;
      B.line(Polyline{at(d, R), at(d, R - (ten ? 15.0 : 8.0))}, w, true);
      if (ten && std::abs(d) > 4) {
        const Vec2 q = at(d, R - 38.0);
        B.text(q.x, q.y + 8, label(h), 24, kMid, true, d);
      }
    }
    B.line(Polyline{{CX, cy - R - 3}, {CX - 9, cy - R - 18}, {CX + 9, cy - R - 18}}, w, true, true);
    B.text(CX, cy - R - 26, fmt("%03lld", now), 26, kMid, true);
    return;
  }
  constexpr double y0 = 95.0, ppd = 6.0, half = 26.0;
  B.line(Polyline{{CX - half * ppd, y0}, {CX + half * ppd, y0}}, w, true);
  const long long base = static_cast<long long>(std::floor((hdg - half) / 5.0)) * 5;
  for (long long h = base; h <= pyint(hdg + half) + 1; h += 5) {
    const double d = h - hdg;
    if (std::abs(d) > half) continue;
    const double x = CX + d * ppd;
    const bool ten = pymod(h, 10LL) == 0;
    B.line(Polyline{{x, y0}, {x, y0 + (ten ? 14.0 : 8.0)}}, w, true);
    if (ten && std::abs(x - CX) > 36) B.text(x, y0 - 8, label(h), 22, kMid, true);
  }
  B.line(Polyline{{CX - 8, y0 + 28}, {CX, y0 + 17}, {CX + 8, y0 + 28}}, w, true);
  B.line(readout_box(CX - 31, CX + 31, y0 - 22, 15.0, 0, 0.0), w, true, true);
  B.text(CX, y0 - 13, fmt("%03lld", now), 24, kMid, true);
}

void misc(Batch& B, const SimState& s, const Settings& set) {
  const double ra = s.radio_alt_ft;
  const double lift = TAPE_BOT - tape_span(set, set.alt_len).bot;  // crosswind mode: follows the shorter tape up
  if (set.show_alt && 0 <= ra && ra <= 2500 && s.on_ground == 0) {
    if (set.sty_alt == "box") B.text(1100.0, TAPE_MID + 114.0, fmt("RA %.0f", ra), 26, kMid, true);
    else B.text(1095.0, 772.0 - lift, fmt("RA %.0f", ra), 26, kMid, true);  // under the tape, clear of the runway
  }
}

// Every airframe-fixed symbol, each group through its own layout transform. Returns the Airbus altitude drum, if
// any (drawn later inside its window).
std::optional<Drum> fixed_symbols(Batch& B, const SimState& p, const HudFilters& filt, const Settings& set,
                                  const Groups& g) {
  B.set_xform(g.spd);
  speed_tape(B, p, filt, set);
  B.set_xform(g.wind);
  ground_speed(B, p, set);
  B.set_xform(g.alt);
  auto d = altitude_tape(B, p, set);
  B.set_xform(g.vs);
  vertical_speed(B, filt, set);
  B.set_xform(g.roll);
  roll_scale(B, p, set);
  B.set_xform(g.att);
  aircraft_ref(B, set);
  B.set_xform(g.hdg);
  heading_fixed(B, p, set);
  B.set_xform(g.alt);
  misc(B, p, set);
  B.set_xform(Xf{});
  return d;
}

}  // namespace

// ---------------------------------------------------------------- Homography
Vec2 Homography::pt(double x, double y) const {
  const double w = m[2][0] * x + m[2][1] * y + m[2][2];
  return {(m[0][0] * x + m[0][1] * y + m[0][2]) / w, (m[1][0] * x + m[1][1] * y + m[1][2]) / w};
}

std::optional<Homography> rect_to_quad(double w, double h, const std::array<Vec2, 4>& q) {
  // unit square -> quad (Heckbert), then scale the rectangle down to the unit square
  const double x0 = q[0].x, y0 = q[0].y, x1 = q[1].x, y1 = q[1].y, x2 = q[2].x, y2 = q[2].y, x3 = q[3].x,
               y3 = q[3].y;
  const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
  double a, b, c, d, e, f, g, hh;
  if (sx == 0.0 && sy == 0.0) {
    a = x1 - x0, b = x2 - x1, c = x0, d = y1 - y0, e = y2 - y1, f = y0, g = 0, hh = 0;
  } else {
    const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    const double den = dx1 * dy2 - dx2 * dy1;
    if (den == 0.0) return std::nullopt;
    g = (sx * dy2 - dx2 * sy) / den;
    hh = (dx1 * sy - sx * dy1) / den;
    a = x1 - x0 + g * x1, b = x3 - x0 + hh * x3, c = x0;
    d = y1 - y0 + g * y1, e = y3 - y0 + hh * y3, f = y0;
  }
  Homography H;
  H.m[0][0] = a / w, H.m[0][1] = b / h, H.m[0][2] = c;
  H.m[1][0] = d / w, H.m[1][1] = e / h, H.m[1][2] = f;
  H.m[2][0] = g / w, H.m[2][1] = hh / h, H.m[2][2] = 1.0;
  return H;
}

// ---------------------------------------------------------------- Batch
void Batch::line(const std::vector<OptVec2>& pts, double w, bool canvas, bool close, bool dash) {
  line(keep(pts), w, canvas, close, dash);
}

Vec2 Batch::map(double x, double y) const {
  const Vec2 q = xf_.apply(x, y);
  return H_ ? H_->pt(q) : q;
}

void Batch::line(const Polyline& in, double w, bool canvas, bool close, bool dash) {
  if (in.size() < 2) return;
  Polyline pts = in;
  if (canvas && H_)
    for (auto& q : pts) q = map(q.x, q.y);
  const double width = (canvas ? w * sc_ : w) * lw_;
  const Key key{round1(std::max(1.2, width)), dash};
  for (auto& [k, items] : lines_) {
    if (k.w == key.w && k.dash == key.dash) {
      items.push_back({std::move(pts), close});
      return;
    }
  }
  lines_.push_back({key, {LineItem{std::move(pts), close}}});
}

void Batch::fill(const Polyline& in, bool canvas) {
  Polyline pts = in;
  if (canvas && H_)
    for (auto& q : pts) q = map(q.x, q.y);
  fills_.push_back(std::move(pts));
}

void Batch::text(double x, double y, const std::wstring& s, double px, Align align, bool canvas, double ang,
                 double dy) {
  if (canvas && H_) {
    const Vec2 q = map(x, y);
    x = q.x, y = q.y;
    px = px * sc_ * TEXT_GAIN * xf_.s;
  }
  texts_.push_back({x, y, px * tk_, s, align, ang, dy * tk_});
}

void Batch::emit(std::vector<double>& cmds, std::wstring& strs) const {
  for (const auto& [k, items] : lines_) {
    for (const auto& it : items) {
      cmds.insert(cmds.end(), {1.0, k.w, k.dash ? 1.0 : 0.0, it.closed ? 1.0 : 0.0, static_cast<double>(it.pts.size())});
      for (const auto& q : it.pts) cmds.insert(cmds.end(), {q.x, q.y});
    }
  }
  for (const auto& f : fills_) {
    cmds.insert(cmds.end(), {2.0, static_cast<double>(f.size())});
    for (const auto& q : f) cmds.insert(cmds.end(), {q.x, q.y});
  }
  for (const auto& t : texts_) {
    cmds.insert(cmds.end(), {3.0, t.x, t.y, t.px, static_cast<double>(t.align), t.ang, t.dy,
                             static_cast<double>(strs.size()), static_cast<double>(t.s.size())});
    strs += t.s;
  }
  cmds.push_back(6.0);
}

void Batch::points(std::vector<Vec2>& out) const {
  for (const auto& [k, items] : lines_)
    for (const auto& it : items) out.insert(out.end(), it.pts.begin(), it.pts.end());
  for (const auto& f : fills_) out.insert(out.end(), f.begin(), f.end());
  for (const auto& t : texts_) {  // a rough box: 0.55 em per character, cap height above the baseline
    const double w = 0.55 * t.px * static_cast<double>(t.s.size());
    const double x0 = t.align == kStart ? t.x : t.align == kMid ? t.x - w / 2 : t.x - w;
    const double y1 = t.y + t.dy + 0.2 * t.px, y0 = t.y + t.dy - 0.8 * t.px;
    out.insert(out.end(), {{x0, y0}, {x0 + w, y0}, {x0 + w, y1}, {x0, y1}});
  }
}

// ---------------------------------------------------------------- scene
void finish(Scene& scene, const Settings& set);  // colour and finish from the settings (below)

namespace {
// The layout differs from the drawing of the reference (the HUD's field is then a little larger).
bool layout_moved(const Settings& s) {
  for (double v : {s.spd_dx, s.spd_dy, s.alt_dx, s.alt_dy, s.vs_dx, s.vs_dy, s.wind_dx, s.wind_dy, s.roll_dy, s.hdg_dy})
    if (v != 0.0) return true;
  for (double v : {s.lay_spread, s.spd_size, s.alt_size, s.vs_size, s.wind_size, s.roll_size, s.att_size, s.text_size})
    if (v != 1.0) return true;
  return s.sty_hdg == "arc" || s.sty_roll == "bottom";
}
}  // namespace

Scene build_scene(const Model& model, const SimState& s, double W, double H, double ox, double oy,
                  const Settings& set, const Runway* runway, const std::wstring& flash, double now,
                  const std::vector<Vec2>* border, const std::wstring& watermark) {
  const Geometry geo = model.geometry(s, W, H, runway);
  const SimState& p = geo.p;
  Frame fr{p, geo.R, camera_to_body(p.cp_yaw, p.cp_pitch, p.cp_roll).transposed(), geo.f, W, H, ox, oy,
           set.hud_scale};
  Conformal conf = conformal(fr, set, p.hdg_true, p.hdg_mag);
  auto sh = [&](const Polyline& pts) {
    Polyline out;
    for (const auto& q : pts) out.push_back({q.x - ox, q.y - oy});
    return out;
  };
  std::optional<Polyline> rp;
  if (geo.runway) rp = sh(*geo.runway);
  if (rp && !rp->empty()) {  // nothing conformal is drawn across the runway
    if (!conf.horizon.empty()) conf.horizon_pieces = clip_segments_outside(conf.horizon, *rp);
    for (auto& rung : conf.rungs) {
      std::vector<Polyline> out;
      for (const auto& seg : rung.segs)
        for (auto& q : clip_segments_outside(seg, *rp)) out.push_back(std::move(q));
      rung.segs = std::move(out);
    }
    std::vector<Polyline> minor;
    for (const auto& seg : conf.minor)
      for (auto& q : clip_segments_outside(seg, *rp)) minor.push_back(std::move(q));
    conf.minor = std::move(minor);
  }
  OptVec2 bird;
  if (geo.bird) bird = Vec2{geo.bird->x - ox, geo.bird->y - oy};
  std::vector<Polyline> glide;
  for (const auto& seg : geo.glide) glide.push_back(sh(seg));
  const std::wstring rwy_label = (runway && rp && !rp->empty()) ? std::wstring(runway->name.begin(), runway->name.end())
                                                                : std::wstring();
  const auto fpa = model.fpa_deg();
  const auto vs_fpm = model.vs_fpm();
  const auto flare = flare_cue(model, s, runway, set, now);

  // ---- hud.build()
  Scene scene;
  scene.has_main = true;
  std::optional<Homography> T;
  {
    std::array<Vec2, 4> dst;
    bool ok = true;
    const double src[4][2] = {{0, 0}, {CW, 0}, {CW, CH}, {0, CH}};
    for (int i = 0; i < 4; ++i) {
      const auto q = fr.canvas_pix(src[i][0], src[i][1]);
      if (!q) {
        ok = false;
        break;
      }
      dst[i] = *q;
    }
    if (ok) {
      T = rect_to_quad(CW, CH, dst);
      scene.clip = Polyline(dst.begin(), dst.end());
      if (layout_moved(set)) {  // things moved outward stay visible: the field grows by 4.4 deg / 3.3 deg a side
        constexpr double mx = 160.0, my = 120.0;
        const double ext[4][2] = {{-mx, -my}, {CW + mx, -my}, {CW + mx, CH + my}, {-mx, CH + my}};
        Polyline wide;
        for (const auto& e : ext)
          if (const auto q = fr.canvas_pix(e[0], e[1])) wide.push_back(*q);
        if (wide.size() == 4) scene.clip = std::move(wide);
      }
    }
  }
  if (border && border->size() >= 3) {  // the user's border: nothing outside it, fading over the last degree inside
    auto sp = project_border(fr, *border);
    if (!sp) sp = Polyline{{-10, -10}, {-9, -10}, {-10, -9}};  // all of it behind the camera: show nothing
    scene.border = std::make_pair(std::move(*sp), fr.f * std::tan(rad(kBorderFeatherDeg)));
  }
  const double sc = fr.screen_per_canvas();
  Batch& B = scene.main = Batch(T, sc);
  B.set_finish(set.line_w, set.text_size);
  const double w = std::max(1.5, 3.0 * sc);
  scene.bore = fr.canvas_pix(CX, BORE_Y);
  finish(scene, set);

  // radio altitude under the bird (Birdie's addition): placed first so the runway label can step out of its way
  struct BirdRa {
    double x, y, px;
    std::wstring txt;
  };
  std::optional<BirdRa> bird_ra;
  if (bird && set.show_bird && set.show_bird_ra && 0 <= p.radio_alt_ft &&
      p.radio_alt_ft <= std::min(2500.0, set.bird_ra_below_ft) && p.on_ground == 0) {
    const double ra = p.radio_alt_ft, r = (13.0 * sc + 2) * set.bird_size;
    // steps as on the Airbus PFD: 10 ft above 50 ft, 5 ft from 10 to 50 ft, 1 ft below 10 ft
    const long long v = ra >= 50 ? pyround(ra / 10.0) * 10 : ra >= 10 ? pyround(ra / 5.0) * 5 : pyround(ra);
    bird_ra = BirdRa{bird->x, bird->y + r + 24 * sc + 6, 24 * sc + 2, fmt("%lld", v)};
  }

  // conformal (screen space)
  if (rp && !rp->empty() && set.show_runway) {
    B.line(*rp, w, false, true);
    for (const auto& m : geo.tdz_marks) B.line(sh(m), std::max(1.2, 1.8 * sc), false, true);
    for (const auto& m : geo.tdz_end) B.fill(sh(m));
    if (!rwy_label.empty()) {
      Vec2 bmax = (*rp)[0];
      double cx = 0;
      for (const auto& q : *rp) {
        if (q.y > bmax.y) bmax = q;
        cx += q.x;
      }
      cx /= rp->size();
      const double lx = 0.5 * (bmax.x + cx), lpx = 22 * sc + 2;
      double ly = bmax.y + 22 * sc + 8;
      if (bird_ra) {  // a far runway puts its label right under the bird: move the label below the RA
        const double half = 0.3 * (lpx * rwy_label.size() + bird_ra->px * bird_ra->txt.size()) + 4;
        if (std::abs(lx - bird_ra->x) < half && std::abs(ly - bird_ra->y) < 0.5 * (lpx + bird_ra->px) + 4)
          ly = bird_ra->y + lpx + 6;
      }
      B.text(lx, ly, rwy_label, lpx);
    }
  }
  if (conf.horizon_pieces && !conf.horizon_pieces->empty()) {
    for (const auto& seg : *conf.horizon_pieces) B.line(seg, w);
  } else if (!conf.horizon.empty()) {
    B.line(conf.horizon, w);
  }
  for (const auto& [a, b] : conf.hdg_ticks) B.line(Polyline{a, b}, std::max(1.3, 2.4 * sc));
  for (const auto& l : conf.hdg_labels) B.text(l.p.x, l.p.y, l.txt, 26 * sc, kMid, false, l.ang, 10 * sc);
  for (const auto& rung : conf.rungs) {
    for (const auto& seg : rung.segs) B.line(seg, w);
    for (const auto& l : rung.labels)
      B.text(l.p.x, l.p.y, l.txt, 26 * sc, l.side < 0 ? kEnd : kStart, false, l.ang, 9 * sc);
  }
  for (const auto& seg : conf.minor) B.line(seg, std::max(1.2, 2.2 * sc));
  if (!glide.empty() && set.show_glide) {
    for (const auto& seg : glide) B.line(seg, w, false, false, true);
    const Vec2 end = glide.back().back();
    B.text(end.x + 6, end.y + 8 * sc, fmt("-%.1f", set.glide_deg), 24 * sc, kStart);
  }
  if (bird && set.show_bird) {
    const double x = bird->x, y = bird->y;
    const double r = (13.0 * sc + 2) * set.bird_size;
    const double bw = std::max(1.8, 3.6 * sc);
    // the flight path symbol: circle radius cr, wings out to `wing`, tail up to `tail` (0 = none)
    const std::string& st = set.sty_fpv;
    double cr = r, wing = 3 * r, tail = 2.1 * r;
    bool wings = true;
    if (st == "boeing") cr = 0.9 * r, wing = 3.3 * r, tail = 1.9 * r;
    else if (st == "fighter") cr = 0.8 * r, wing = 2.5 * r, tail = 2.0 * r;
    else if (st == "minimal") cr = 0.85 * r, wing = 2.0 * r, tail = 0;
    else if (st == "ring") wing = 1.6 * r, tail = 0, wings = false;
    else if (st == "cross") cr = 0.7 * r, wing = 1.7 * r, tail = 0, wings = false;
    Polyline circ;
    for (double a : linspace(0, 2 * kPi, 25)) circ.push_back({x + cr * std::cos(a), y + cr * std::sin(a)});
    B.line(circ, bw, false, true);
    if (wings) {
      B.line(Polyline{{x - cr, y}, {x - wing, y}}, bw);
      B.line(Polyline{{x + cr, y}, {x + wing, y}}, bw);
    }
    if (tail > 0) B.line(Polyline{{x, y - cr}, {x, y - tail}}, bw);
    if (st == "ring") {  // a dot in the middle
      Polyline dot;
      for (double a : linspace(0, 2 * kPi, 9)) dot.push_back({x + 0.16 * r * std::cos(a), y + 0.16 * r * std::sin(a)});
      B.fill(dot);
    } else if (st == "cross") {  // four short marks around a small ring
      for (int k = 0; k < 4; ++k) {
        const double a = k * kPi / 2, ca = std::cos(a), sa = std::sin(a);
        B.line(Polyline{{x + cr * ca, y + cr * sa}, {x + 1.7 * r * ca, y + 1.7 * r * sa}}, bw);
      }
    }
    // flight path acceleration (Boeing HGS): a caret off the left wing, above it when gaining speed; 1 kt/s is about
    // 3 degrees (along-path acceleration / g, in radians)
    double caret = 0;
    if (set.show_accel) {
      const double acc = model.filters().rate * 0.514444 / 9.80665;  // kt/s -> g
      const double off = std::clamp(deg(acc), -6.0, 6.0) * fr.f * kPi / 180.0;
      const double cx = x - wing - 5 * sc - 2, cy = y - off, k = 9 * sc + 2;
      B.line(Polyline{{cx - k, cy - k}, {cx, cy}, {cx - k, cy + k}}, bw);
      caret = k + 5 * sc + 2;
    }
    if (fpa && set.show_fpa_num) {  // current flight path angle, right of the wing
      const double v = round1(*fpa);
      B.text(x + wing + 7, y + 8 * sc + 1, (v < 0 ? kMinus : L"") + fmt("%.1f", std::abs(v)), 24 * sc + 2, kStart);
    }
    // flare helper, blinking: + on the profile; ++ still steeper than it (keep pulling), or with flare_pull off (the
    // reference) flatter than it
    if (flare && flare->on && (flare->n == 1 || flare->n == (set.flare_pull ? 0 : 2))) {
      const double arm = 0.55 * r;
      const double yc = y - (tail > 0 ? tail : cr + 0.4 * r) - 10 * sc - arm - 2;
      std::vector<double> xs;
      if (flare->n == 1) xs = {x};
      else xs = {x - arm - 5 * sc - 2, x + arm + 5 * sc + 2};
      for (double xc : xs) {
        B.line(Polyline{{xc - arm, yc}, {xc + arm, yc}}, bw);
        B.line(Polyline{{xc, yc - arm}, {xc, yc + arm}}, bw);
      }
    }
    if (vs_fpm && set.show_vs_num) {  // vertical speed (fpm), left of the wing
      const long long v = pyround(*vs_fpm / 10.0) * 10;
      B.text(x - wing - 7 - caret, y + 8 * sc + 1, (v < 0 ? kMinus : L"") + fmt("%lld", std::llabs(v)), 24 * sc + 2, kEnd);
    }
    if (bird_ra) B.text(bird_ra->x, bird_ra->y, bird_ra->txt, bird_ra->px, kMid);
  }

  // airframe-fixed (canvas space)
  if (T) {
    const Groups g = groups(set);
    const auto d = fixed_symbols(B, p, model.filters(), set, g);
    if (!watermark.empty()) {  // test builds: over the top of the altitude tape, wherever the layout puts it, even
      B.set_xform(g.alt);      // with the tape turned off
      B.text(1095.0, tape_span(set, set.alt_len).top - 18.0, watermark, 22, kMid, true);
      B.set_xform(Xf{});
    }
    if (d) {  // rolling tens of the altitude readout: clipped to its window
      Batch D(T, sc);
      D.set_finish(set.line_w, set.text_size);
      D.set_xform(g.alt);
      for (const auto& dg : d->digits) D.text(dg.x, dg.y, dg.txt, 26, kMid, true);
      Polyline win;
      for (const auto& q : d->rect) win.push_back(D.map(q.x, q.y));
      scene.drum = std::make_pair(std::move(win), std::move(D));
    }
  }
  if (!flash.empty()) scene.flash = flash_scene(flash).flash;
  return scene;
}

const wchar_t* font_family(const std::string& f) {
  if (f == "consolas") return L"Consolas";
  if (f == "segoe") return L"Segoe UI Semibold";
  if (f == "narrow") return L"Arial Narrow";
  return L"Bahnschrift";
}

// "#rrggbb" -> 0..1 (the default green when malformed; settings_from_json already checks)
void parse_color(const std::string& c, double rgb[3]) {
  if (c.size() == 7 && c[0] == '#') {
    for (int i = 0; i < 3; ++i) rgb[i] = std::strtol(c.substr(1 + 2 * i, 2).c_str(), nullptr, 16) / 255.0;
    return;
  }
  rgb[0] = 0.0, rgb[1] = 1.0, rgb[2] = 0.0;
}

void finish(Scene& scene, const Settings& set) {
  parse_color(set.hud_color, scene.rgb);
  scene.outline = set.hud_outline;
  scene.glow = set.hud_glow;
}

Scene flash_scene(const std::wstring& flash) {
  Scene s;
  Batch F;
  F.text(14, 26, flash, 15, kStart);
  s.flash = std::move(F);
  return s;
}

CommandList commands(const Scene& scene, double opacity) {
  CommandList out;
  append_commands(out, scene, opacity);
  return out;
}

void append_commands(CommandList& out, const Scene& scene, double opacity) {
  auto& c = out.cmds;
  const double a = std::max(0.05, std::min(1.0, opacity));
  const double* k = scene.rgb;
  if (scene.glow > 0 && scene.has_main) {  // glow: everything once more underneath, wide and faint in the HUD colour
    const double g = std::min(1.0, scene.glow) * a;
    c.insert(c.end(), {10.0, k[0], k[1], k[2], 0.22 * g, k[0], k[1], k[2], 0.13 * g, k[0], k[1], k[2], 0.10 * g, 9.0});
    scene.main.emit(c, out.text);
  }
  // HUD colour (straight alpha: Direct2D premultiplies brush colours itself), outline black 190/255, text halo
  // 120/255 (both times hud_outline)
  const double o = scene.outline;
  c.insert(c.end(), {10.0, k[0], k[1], k[2], a, 0.0, 0.0, 0.0, 0.745 * a * o, 0.0, 0.0, 0.0, 0.47 * a * o, 2.4});
  auto clip = [&](const Polyline& pts) {
    c.insert(c.end(), {4.0, static_cast<double>(pts.size())});
    for (const auto& q : pts) c.insert(c.end(), {q.x, q.y});
  };
  if (scene.has_main) {
    if (scene.clip) clip(*scene.clip);
    if (scene.border) {  // 7 feathered polygon clip: feather px, n, points
      const auto& [pts, feather] = *scene.border;
      c.insert(c.end(), {7.0, feather, static_cast<double>(pts.size())});
      for (const auto& q : pts) c.insert(c.end(), {q.x, q.y});
    }
    scene.main.emit(c, out.text);
    if (scene.drum) {
      clip(scene.drum->first);
      scene.drum->second.emit(c, out.text);
      c.push_back(5.0);
    }
    if (scene.border) c.push_back(5.0);
    if (scene.clip) c.push_back(5.0);
  }
  if (scene.flash) scene.flash->emit(c, out.text);
}

// ---------------------------------------------------------------- HUD border
std::optional<Polyline> border_to_screen(const Model& model, const SimState& s, double W, double H, double ox,
                                         double oy, const std::vector<Vec2>& pts) {
  return project_border(frame_for(model, s, W, H, ox, oy, 1.0), pts);
}

std::vector<OptVec2> border_vertices(const Model& model, const SimState& s, double W, double H, double ox, double oy,
                                     const std::vector<Vec2>& pts) {
  const Frame fr = frame_for(model, s, W, H, ox, oy, 1.0);
  std::vector<OptVec2> out;
  for (const auto& p : pts) {
    const Vec3 q = border_cam(fr, p);
    if (q.z < kBorderNear) out.push_back(std::nullopt);
    else out.push_back(Vec2{W / 2 + fr.f * q.x / q.z - ox, H / 2 - fr.f * q.y / q.z - oy});
  }
  return out;
}

OptVec2 screen_to_border(const Model& model, const SimState& s, double W, double H, double ox, double oy, Vec2 px) {
  const Frame fr = frame_for(model, s, W, H, ox, oy, 1.0);
  const Vec3 cam{(px.x + ox - W / 2) / fr.f, -(px.y + oy - H / 2) / fr.f, 1.0};
  const Vec3 b = fr.R_bc.tmul(cam);  // camera -> body (R_bc is a rotation)
  if (b.z <= 1e-3) return std::nullopt;
  return Vec2{deg(b.x / b.z), deg(b.y / b.z)};
}

std::vector<Vec2> fixed_symbol_points(const Model& model, const SimState& s, const Settings& set) {
  const Geometry geo = model.geometry(s, 1000, 1000, nullptr);
  const SimState& p = geo.p;
  Homography I;  // canvas -> canvas
  I.m[0][0] = I.m[1][1] = I.m[2][2] = 1.0;
  Batch B(I, 1.0);
  const Groups g = groups(set);
  const auto d = fixed_symbols(B, p, model.filters(), set, g);
  std::vector<Vec2> canvas;
  B.points(canvas);
  if (d)
    for (const auto& q : d->rect) canvas.push_back(g.alt.apply(q.x, q.y));
  std::vector<Vec2> out;
  out.reserve(canvas.size());
  for (const auto& q : canvas) out.push_back({(q.x - CX) / ONE_DEG, (BORE_Y - q.y) / ONE_DEG});
  return out;
}

void append_edit(CommandList& out, const EditView& e) {
  auto& c = out.cmds;
  const double a = std::clamp(e.alpha, 0.0, 1.0);
  // the panel's palette: apricot #f2b48a, ink #f2e7df, mute #a8979a, dusk #1c1518
  static constexpr double AP[3] = {0.949, 0.706, 0.541}, INK[3] = {0.949, 0.906, 0.875},
                          MUTE[3] = {0.659, 0.592, 0.604}, DUSK[3] = {0.110, 0.082, 0.094};
  auto style = [&](const double* col, double alpha, double outline_alpha, double outline_w) {
    c.insert(c.end(), {10.0, col[0], col[1], col[2], alpha, DUSK[0], DUSK[1], DUSK[2], outline_alpha, DUSK[0], DUSK[1],
                       DUSK[2], 0.6 * a, outline_w});
  };
  auto veil = [&](double alpha, const Polyline* hole) {  // 8 fill outside a polygon: rgba, n, points
    c.insert(c.end(), {8.0, DUSK[0], DUSK[1], DUSK[2], alpha, hole ? static_cast<double>(hole->size()) : 0.0});
    if (hole)
      for (const auto& q : *hole) c.insert(c.end(), {q.x, q.y});
  };
  auto poly = [&](const Polyline& pts, double w, bool closed, bool dash) {
    c.insert(c.end(), {1.0, w, dash ? 1.0 : 0.0, closed ? 1.0 : 0.0, static_cast<double>(pts.size())});
    for (const auto& q : pts) c.insert(c.end(), {q.x, q.y});
  };
  auto circle = [](Vec2 o, double r) {
    Polyline p;
    for (double t : linspace(0, 2 * kPi, 33)) p.push_back({o.x + r * std::cos(t), o.y + r * std::sin(t)});
    return p;
  };
  auto fill = [&](const Polyline& pts) {
    c.insert(c.end(), {2.0, static_cast<double>(pts.size())});
    for (const auto& q : pts) c.insert(c.end(), {q.x, q.y});
  };
  auto text = [&](double x, double y, double px, const std::wstring& s) {
    c.insert(c.end(),
             {3.0, x, y, px, 1.0, 0.0, 0.0, static_cast<double>(out.text.size()), static_cast<double>(s.size())});
    out.text += s;
  };

  veil(0.004, nullptr);  // the whole window takes the mouse while editing
  if (e.poly.size() >= 3) veil(0.55 * a, &e.poly);
  // outline
  style(AP, a, 0.45 * a, 1.6);
  if (e.poly.size() >= 3) poly(e.poly, 1.3, true, false);
  c.push_back(6.0);
  // handles: dusk-filled rings; the one under the mouse filled apricot with a soft ring around it
  const int hot = e.drag >= 0 ? e.drag : e.hover;
  const bool hot_ok = hot >= 0 && hot < static_cast<int>(e.handles.size()) && e.handles[hot];
  style(DUSK, 0.9 * a, 0.0, 1.0);
  for (size_t i = 0; i < e.handles.size(); ++i)
    if (e.handles[i] && static_cast<int>(i) != hot) fill(circle(*e.handles[i], 5.0));
  c.push_back(6.0);
  style(AP, a, 0.45 * a, 1.6);
  for (size_t i = 0; i < e.handles.size(); ++i)
    if (e.handles[i] && static_cast<int>(i) != hot) poly(circle(*e.handles[i], 5.0), 1.3, true, false);
  if (hot_ok) fill(circle(*e.handles[hot], 5.5));
  if (e.add_at && hot < 0) poly(circle(*e.add_at, 5.0), 1.2, true, true);
  c.push_back(6.0);
  if (hot_ok) {
    style(AP, 0.35 * a, 0.0, 1.0);
    poly(circle(*e.handles[hot], 11.0), 1.2, true, false);
    c.push_back(6.0);
  }
  // the hint, low in the window
  const double y = e.H - std::max(48.0, e.H * 0.07);
  style(INK, 0.95 * a, 0.0, 1.0);
  if (!e.line1.empty()) text(e.W / 2, y, 21, e.line1);
  c.push_back(6.0);
  style(MUTE, 0.95 * a, 0.0, 1.0);
  if (!e.line2.empty()) text(e.W / 2, y + 28, 15, e.line2);
  c.push_back(6.0);
}

}  // namespace birdie::hud
