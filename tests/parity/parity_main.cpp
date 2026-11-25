// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Parity runner: replays every fixture through the C++ core and compares with the reference's golden output.
//   inputs   : each golden input state matches the replayed stream
//   geometry : bird, focal, runway polygon, FPA/VS, flare cue (S1)
// Usage: birdie_parity [fixtures_root]
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

#include "core/camera.h"
#include "core/geo.h"
#include "core/model.h"
#include "core/runway.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/version.h"
#include "fixture.h"
#include "hud/scene.h"
#include "png.h"
#include "render/d2d.h"

namespace fs = std::filesystem;
using birdie::test::Fixture;
using nlohmann::json;

namespace {

// The golden state is the unrounded reference input; input.csv keeps 6 decimals.
constexpr double kStateTol = 1e-5;

struct Tally {
  int checks = 0, failures = 0;
  std::string first_fail;
  void fail(const char* fmt, ...) {
    ++failures;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("        FAIL %s\n", buf);
  }
};

birdie::SimState state_from_row(const Fixture& fx, size_t row) {
  birdie::SimState s;
  for (const auto& v : birdie::kVars)
    if (!birdie::not_in_reference(v.key)) s.*v.field = fx.at(row, v.key);
  s.ra_raw_ft = fx.at(row, "ra_raw_ft");
  birdie::camera_from_chaseplane(s);
  return s;
}

birdie::SimState state_from_json(const json& j) {
  birdie::SimState s;
  for (const auto& v : birdie::kVars)
    if (!birdie::not_in_reference(v.key)) s.*v.field = j.at(v.key).get<double>();
  s.ra_raw_ft = j.value("ra_raw_ft", s.radio_alt_ft);
  birdie::camera_from_chaseplane(s);
  return s;
}

double num(const json& j) { return j.is_string() ? std::stod(j.get<std::string>()) : j.get<double>(); }

std::optional<birdie::Runway> runway_from_fixture(const json& meta) {
  if (!meta.contains("runway") || meta["runway"].is_null()) return std::nullopt;
  const json& r = meta["runway"];
  // the reference builds the test runway as Runway("TEST", lat, lon, alt, hdg, length, width, name), slope 0
  return birdie::make_runway("TEST", num(r.at("lat")), num(r.at("lon")), num(r.at("alt_m")),
                             num(r.at("landing_hdg_true")), num(r.at("length_m")), num(r.at("width_m")),
                             r.at("name").get<std::string>());
}

// Coordinates far outside the screen (near-plane clipped corners) are ill-conditioned: allow a relative part.
// 3e-6: the port's focal length differs from the reference's by ~1.3e-6 relative at the fixture's zoom (core/geo.h),
// which moves a corner 55 000 px off screen by ~0.07 px.
bool close_px(double a, double b, double tol) { return std::abs(a - b) <= tol + 3e-6 * std::max(std::abs(a), std::abs(b)); }

void compare_geometry(const json& gold, const birdie::Geometry& g, const birdie::Model& m,
                      const std::optional<birdie::FlareCue>& flare, double tol_px, Tally& t, double& worst_px) {
  // bird
  const json& gb = gold.at("bird_px");
  if (gb.is_null() != !g.bird) {
    t.fail("bird presence: golden %s, port %s", gb.is_null() ? "none" : "yes", g.bird ? "yes" : "none");
  } else if (g.bird) {
    const double dx = g.bird->x - gb[0].get<double>(), dy = g.bird->y - gb[1].get<double>();
    worst_px = std::max({worst_px, std::abs(dx), std::abs(dy)});
    if (std::abs(dx) > tol_px || std::abs(dy) > tol_px) t.fail("bird off by (%.4f, %.4f) px", dx, dy);
  }
  // focal
  // The port maps zoom -> vertical FOV (measured 11/15, core/geo.h) instead of the reference's linear focal; both are
  // anchored at zoom 0.49725 -> 1298 px, so near the fixture's zoom they agree to a few thousandths of a pixel.
  if (std::abs(g.f - gold.at("focal_px").get<double>()) > 0.01) t.fail("focal %.6f vs %.6f", g.f, gold["focal_px"].get<double>());
  // runway polygon
  const json& gr = gold.at("runway_px");
  if (gr.is_null() != !g.runway) {
    t.fail("runway presence differs");
  } else if (g.runway) {
    if (gr.size() != g.runway->size()) {
      t.fail("runway polygon: %zu points vs %zu", g.runway->size(), gr.size());
    } else {
      for (size_t i = 0; i < gr.size(); ++i) {
        const double x = gr[i][0].get<double>(), y = gr[i][1].get<double>();
        const auto& q = (*g.runway)[i];
        if (std::abs(x) < 1e4 && std::abs(y) < 1e4)
          worst_px = std::max({worst_px, std::abs(q.x - x), std::abs(q.y - y)});
        if (!close_px(q.x, x, tol_px) || !close_px(q.y, y, tol_px))
          t.fail("runway corner %zu: (%.4f, %.4f) vs (%.4f, %.4f)", i, q.x, q.y, x, y);
      }
    }
  }
  // FPA / VS
  auto opt_cmp = [&](const char* what, const json& gv, std::optional<double> v, double tol) {
    if (gv.is_null() != !v) t.fail("%s presence differs", what);
    else if (v && std::abs(*v - gv.get<double>()) > tol) t.fail("%s %.6f vs %.6f", what, *v, gv.get<double>());
  };
  opt_cmp("fpa_deg", gold.at("fpa_deg"), m.fpa_deg(), 1e-4);
  opt_cmp("vs_fpm", gold.at("vs_fpm"), m.vs_fpm(), 1e-2);
  // flare cue
  const json& gf = gold.at("flare");
  if (gf.is_null() != !flare) {
    t.fail("flare presence: golden %s, port %s", gf.is_null() ? "none" : "yes", flare ? "yes" : "none");
  } else if (flare) {
    if (flare->n != gf.at("n").get<int>() || flare->on != gf.at("on").get<bool>())
      t.fail("flare n/on %d/%d vs %d/%d", flare->n, flare->on, gf["n"].get<int>(), gf["on"].get<bool>());
    if (std::abs(flare->rel - gf.at("rel").get<double>()) > 1e-4 ||
        std::abs(flare->target - gf.at("target").get<double>()) > 1e-4)
      t.fail("flare rel/target %.5f/%.5f vs %.5f/%.5f", flare->rel, flare->target, gf["rel"].get<double>(),
             gf["target"].get<double>());
  }
}

std::string narrow(const std::wstring& w) {
  std::string out;
  for (wchar_t c : w) {
    if (c < 0x80) out += static_cast<char>(c);
    else out += "\\u" + std::to_string(static_cast<int>(c));
  }
  return out;
}

std::wstring widen_utf8(const std::string& s) {
  std::wstring out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i];
    unsigned cp;
    int n;
    if (c < 0x80) cp = c, n = 1;
    else if ((c >> 5) == 6) cp = c & 0x1F, n = 2;
    else if ((c >> 4) == 14) cp = c & 0x0F, n = 3;
    else cp = c & 0x07, n = 4;
    for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
    out += static_cast<wchar_t>(cp);  // BMP only (the HUD strings are)
    i += n;
  }
  return out;
}

// Same ops and counts; floats within tol (the golden list is rounded to 1e-4).
void compare_commands(const json& gold_cmds, const std::wstring& gold_text, const birdie::hud::CommandList& cl,
                      double tol, Tally& t, double& worst) {
  if (gold_text != cl.text) {
    t.fail("string pool differs:\n          golden %s\n          port   %s", narrow(gold_text).c_str(),
           narrow(cl.text).c_str());
  }
  if (gold_cmds.size() != cl.cmds.size()) t.fail("command list: %zu floats vs %zu", cl.cmds.size(), gold_cmds.size());
  const size_t n = std::min(gold_cmds.size(), cl.cmds.size());
  int reported = 0;
  for (size_t i = 0; i < n; ++i) {
    const double g = gold_cmds[i].get<double>(), v = cl.cmds[i];
    const double d = std::abs(g - v);
    if (std::abs(g) < 1e4) worst = std::max(worst, d);
    if (!close_px(v, g, tol) && reported++ < 5) t.fail("cmd[%zu] = %.5f vs %.5f", i, v, g);
  }
}

// Direct2D anti-aliasing may differ in the last bit (float32 rounding of coordinates): allow small per-channel
// differences on a few pixels, nothing visible.
constexpr int kPixelTol = 4;            // per channel, 0..255
constexpr double kMaxBadFraction = 1e-4;  // pixels over kPixelTol
constexpr int kPixelHard = 48;          // no pixel may differ more than this

birdie::render::D2DRenderer& renderer() {
  static birdie::render::D2DRenderer r;
  static bool ok = [] {
    if (!r.init_offscreen()) {
      std::printf("FAIL: renderer init: %s\n", narrow(r.error()).c_str());
      return false;
    }
    return true;
  }();
  (void)ok;
  return r;
}

void compare_image(const Fixture& fx, const birdie::test::Check& ck, const birdie::hud::CommandList& cl, int bw,
                   int bh, Tally& t) {
  const int before = t.failures;
  birdie::test::Image gold;
  if (!birdie::test::load_png_rgba(ck.golden_png, gold)) {
    t.fail("cannot read %s", ck.golden_png.string().c_str());
    return;
  }
  if (gold.w != bw || gold.h != bh) {
    t.fail("golden image is %dx%d, box is %dx%d", gold.w, gold.h, bw, bh);
    return;
  }
  std::vector<std::uint8_t> bgra;
  if (!renderer().render_to_memory(cl, bw, bh, bgra)) {
    t.fail("render_to_memory failed");
    return;
  }
  birdie::test::Image port{bw, bh, std::vector<std::uint8_t>(bgra.size())}, diff{bw, bh, {}};
  diff.px.assign(bgra.size(), 0);
  size_t bad = 0, differing = 0;
  int worst = 0;
  for (size_t i = 0; i < bgra.size(); i += 4) {
    port.px[i] = bgra[i + 2], port.px[i + 1] = bgra[i + 1], port.px[i + 2] = bgra[i], port.px[i + 3] = bgra[i + 3];
    int d = 0;
    for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(int(port.px[i + c]) - int(gold.px[i + c])));
    worst = std::max(worst, d);
    if (d > 0) ++differing;
    if (d > kPixelTol) ++bad;
    const std::uint8_t v = static_cast<std::uint8_t>(std::min(255, d * 8));
    diff.px[i] = v, diff.px[i + 1] = 0, diff.px[i + 2] = d ? 255 - v : 0, diff.px[i + 3] = d ? 255 : 0;
  }
  const size_t n = static_cast<size_t>(bw) * bh;
  if (worst > kPixelHard) t.fail("a pixel differs by %d", worst);
  if (bad > n * kMaxBadFraction) t.fail("%zu pixels differ by more than %d", bad, kPixelTol);
  std::printf("        image    %s (%dx%d, %zu px differ, %zu over %d, worst %d)\n",
              t.failures == before ? "ok" : "FAILED", bw, bh, differing, bad, kPixelTol, worst);
  if (t.failures != before || differing) {
    const fs::path out = fs::path(BIRDIE_PARITY_OUT) / fx.name;
    fs::create_directories(out);
    char idx[16];
    std::snprintf(idx, sizeof idx, "%03d", ck.index);
    birdie::test::save_png_rgba(out / (std::string(idx) + "_port.png"), port);
    birdie::test::save_png_rgba(out / (std::string(idx) + "_diff.png"), diff);
  }
}

// ---- HUD border (Birdie's own, docs/HUD_BORDER.md): the reference has none, so this checks the rendering against
// the same frame without a border: nothing outside the polygon, untouched well inside it, only fading in between.
// Writes NNN_border.png (over a dark backdrop) and NNN_edit.png (the editor) for a look.
bool inside_px(const birdie::Polyline& poly, double x, double y) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const auto a = poly[i], b = poly[j];
    if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
  }
  return in;
}

double edge_px(const birdie::Polyline& poly, double x, double y) {
  double best = 1e300;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const auto a = poly[j];
    const double dx = poly[i].x - a.x, dy = poly[i].y - a.y, l2 = dx * dx + dy * dy;
    const double t = l2 > 0 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
    best = std::min(best, std::hypot(x - (a.x + t * dx), y - (a.y + t * dy)));
  }
  return best;
}

birdie::test::Image to_rgba(const std::vector<std::uint8_t>& bgra, int w, int h, std::uint8_t backdrop) {
  birdie::test::Image img{w, h, std::vector<std::uint8_t>(bgra.size())};
  for (size_t i = 0; i < bgra.size(); i += 4) {  // premultiplied over an opaque backdrop
    const int a = bgra[i + 3];
    img.px[i] = static_cast<std::uint8_t>(bgra[i + 2] + backdrop * (255 - a) / 255);
    img.px[i + 1] = static_cast<std::uint8_t>(bgra[i + 1] + backdrop * (255 - a) / 255);
    img.px[i + 2] = static_cast<std::uint8_t>(bgra[i] + backdrop * (255 - a) / 255);
    img.px[i + 3] = 255;
  }
  return img;
}

void check_border(const Fixture& fx, const birdie::test::Check& ck, const birdie::Model& model,
                  const birdie::SimState& s, const birdie::Settings& set, const birdie::Runway* runway, double W,
                  double H, double ox, double oy, int bw, int bh, double now, Tally& t) {
  const int before = t.failures;
  // a traced windscreen: lower edge cutting the tapes' feet, one slanted corner
  const std::vector<birdie::Vec2> border{{-16, 8}, {16, 8.5}, {15, -6}, {3, -9.5}, {-15, -7}};
  const auto plain = birdie::hud::build_scene(model, s, W, H, ox, oy, set, runway, L"", now);
  const auto with = birdie::hud::build_scene(model, s, W, H, ox, oy, set, runway, L"", now, &border);
  if (!with.border) {
    t.fail("border: no border in the scene");
    return;
  }
  const auto& poly = with.border->first;
  const double feather = with.border->second;
  std::vector<std::uint8_t> a, b;
  if (!renderer().render_to_memory(birdie::hud::commands(plain, set.opacity), bw, bh, a) ||
      !renderer().render_to_memory(birdie::hud::commands(with, set.opacity), bw, bh, b)) {
    t.fail("border: render failed");
    return;
  }
  size_t outside_lit = 0, inner_changed = 0, brighter = 0, faded = 0;
  int inner_worst = 0;
  for (int y = 0; y < bh; ++y) {
    for (int x = 0; x < bw; ++x) {
      const size_t i = (static_cast<size_t>(y) * bw + x) * 4;
      const double cx = x + 0.5, cy = y + 0.5;
      const bool in = inside_px(poly, cx, cy);
      const double e = edge_px(poly, cx, cy);
      const int pa = a[i + 3], pb = b[i + 3];
      if (!in && e > 1.0 && pb > 0) ++outside_lit;
      if (in && e > 1.6 * feather) {
        int d = 0;
        for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(int(a[i + c]) - int(b[i + c])));
        // a layer composites overlapping outlines a touch differently (a few px, ~12/255 alpha): the image tolerances
        if (d > kPixelTol) ++inner_changed;
        inner_worst = std::max(inner_worst, d);
      }
      if (pb > pa + 2) ++brighter;
      if (in && e < feather && pa > 64 && pb < pa - 16) ++faded;
    }
  }
  if (outside_lit) t.fail("border: %zu pixels drawn outside it", outside_lit);
  if (inner_changed > static_cast<size_t>(bw) * bh * kMaxBadFraction || inner_worst > kPixelHard)
    t.fail("border: %zu pixels changed well inside it (worst %d)", inner_changed, inner_worst);
  if (brighter) t.fail("border: %zu pixels brighter than without it", brighter);
  if (!faded) t.fail("border: nothing fades along the edge (feather %.1f px)", feather);
  std::printf("        border   %s (feather %.1f px, %zu px faded, %zu inside differ, worst %d)\n",
              t.failures == before ? "ok" : "FAILED", feather, faded, inner_changed, inner_worst);

  // the editor over the faint HUD, for the eye only
  birdie::hud::CommandList cl = birdie::hud::commands(with, set.opacity * 0.28);
  birdie::hud::EditView ev;
  ev.poly = poly;
  ev.handles = birdie::hud::border_vertices(model, s, W, H, ox, oy, border);
  ev.hover = 3;
  ev.W = bw, ev.H = bh;
  ev.line1 = L"沿着窗框，把小点拖过去";
  ev.line2 = L"点边线加一点 · 右键删点 · Enter 存好 · Esc 不改";
  if (ev.handles[0] && ev.handles[1]) ev.add_at = 0.5 * (*ev.handles[0] + *ev.handles[1]);
  birdie::hud::append_edit(cl, ev);
  std::vector<std::uint8_t> e;
  const fs::path out = fs::path(BIRDIE_PARITY_OUT) / fx.name;
  fs::create_directories(out);
  char idx[16];
  std::snprintf(idx, sizeof idx, "%03d", ck.index);
  birdie::test::save_png_rgba(out / (std::string(idx) + "_border.png"), to_rgba(b, bw, bh, 70));
  if (renderer().render_to_memory(cl, bw, bh, e))
    birdie::test::save_png_rgba(out / (std::string(idx) + "_edit.png"), to_rgba(e, bw, bh, 110));
}

// ---- background tint (settings tint_*): the colour fills the field (combiner, cut by the border), nothing outside;
// with a soft edge it fades towards the combiner's edge. Checked where no symbol is drawn.
void check_tint(const birdie::Model& model, const birdie::SimState& s, birdie::Settings set,
                const birdie::Runway* runway, double W, double H, double ox, double oy, int bw, int bh, double now,
                Tally& t) {
  const int before = t.failures;
  const std::vector<birdie::Vec2> border{{-16, 8}, {16, 8.5}, {15, -6}, {3, -9.5}, {-15, -7}};
  const auto plain = birdie::hud::build_scene(model, s, W, H, ox, oy, set, runway, L"", now, &border);
  const auto open = birdie::hud::build_scene(model, s, W, H, ox, oy, set, runway, L"", now);  // no border
  set.tint_on = true, set.tint_color = "#204080", set.tint_alpha = 0.2, set.tint_edge = "hard";
  const auto hard = birdie::hud::build_scene(model, s, W, H, ox, oy, set, runway, L"", now, &border);
  set.tint_edge = "feather";
  const auto soft = birdie::hud::build_scene(model, s, W, H, ox, oy, set, runway, L"", now);
  if (!plain.clip || !hard.border || !hard.tint || !soft.tint) {
    t.fail("tint: no field / border / tint in the scene");
    return;
  }
  std::vector<std::uint8_t> a, o, b, c;
  if (!renderer().render_to_memory(birdie::hud::commands(plain, set.opacity), bw, bh, a) ||
      !renderer().render_to_memory(birdie::hud::commands(open, set.opacity), bw, bh, o) ||
      !renderer().render_to_memory(birdie::hud::commands(hard, set.opacity), bw, bh, b) ||
      !renderer().render_to_memory(birdie::hud::commands(soft, set.opacity), bw, bh, c)) {
    t.fail("tint: render failed");
    return;
  }
  const auto& field = *plain.clip;
  const auto& poly = hard.border->first;
  const double feather = hard.border->second, soft_px = soft.tint->feather_px;
  const int want_a = static_cast<int>(std::lround(255 * 0.2 * set.opacity));
  const int want_b = static_cast<int>(std::lround(0x80 * 0.2 * set.opacity));
  size_t outside = 0, deep = 0, deep_bad = 0, rim = 0, rim_bad = 0;
  for (int y = 0; y < bh; ++y) {
    for (int x = 0; x < bw; ++x) {
      const size_t i = (static_cast<size_t>(y) * bw + x) * 4;
      const double cx = x + 0.5, cy = y + 0.5;
      const bool in_f = inside_px(field, cx, cy), in_b = inside_px(poly, cx, cy);
      if ((!in_f && edge_px(field, cx, cy) > 1.0) || (!in_b && edge_px(poly, cx, cy) > 1.0))
        if (b[i + 3] > 0) ++outside;
      if (in_f && o[i + 3] == 0 && edge_px(field, cx, cy) < 0.15 * soft_px) {  // no border, no symbol here
        ++rim;
        if (c[i + 3] > want_a / 2) ++rim_bad;
      }
      if (a[i + 3] != 0) continue;  // a symbol (or its fade) is here
      if (in_f && in_b && edge_px(field, cx, cy) > 2 && edge_px(poly, cx, cy) > 1.6 * feather) {
        ++deep;
        if (std::abs(b[i + 3] - want_a) > 2 || std::abs(b[i] - want_b) > 2) ++deep_bad;
      }
    }
  }
  if (outside) t.fail("tint: %zu pixels drawn outside the field", outside);
  if (!deep || deep_bad > deep / 1000) t.fail("tint: %zu of %zu pixels inside not the tint colour", deep_bad, deep);
  if (!rim || rim_bad) t.fail("tint: %zu of %zu pixels at the edge not faded (feather %.1f px)", rim_bad, rim, soft_px);
  std::printf("        tint     %s (%zu px tinted, %zu px faded at the edge)\n", t.failures == before ? "ok" : "FAILED",
              deep, rim);
}

int run_fixture(const Fixture& fx) {
  Tally t;
  const json& meta = fx.meta;
  birdie::Settings set = birdie::settings_from_json(meta.at("settings"));
  set.show_bird_ra = false;  // Birdie's own addition: the reference never drew it
  set.sty_roll = "a32nx";     // the reference drew the FlyByWire PFD roll scale (internal only, not offered to users)
  set.sty_speed = "a32nx";    // ... and its speed index, trend arrow, labels and ground speed (see scene.cpp)
  set.sty_alt = "a32nx";      // ... its altitude labels
  set.sty_hdg = "a32nx";      // ... its one-digit heading labels
  set.flare_pull = false;     // the reference's ++ = flatter than the profile
  set.show_tdz = false;       // touchdown zone marks: Birdie's own
  const auto runway = runway_from_fixture(meta);
  const double W = meta.at("client_px")[0].get<double>(), H = meta.at("client_px")[1].get<double>();
  const double hz = meta.at("input").value("hz", 30.0);
  const double tol_px = meta.value("compare", json::object()).value("geometry_tol_px", 0.05);
  // 2e-3: the golden output is rounded to 4 decimals (1e-3 covered that), and the port's measured zoom model
  // (core/geo.h) moves far-off points by about another 1e-3 px against the reference's linear focal.
  const double tol_cmd = std::max(2e-3, meta.value("compare", json::object()).value("commands_tol", 1e-3));

  birdie::Model model(set);
  size_t next = 0;
  for (size_t row = 0; row < fx.rows.size() && next < fx.checks.size(); ++row) {
    // replay clock = row / hz (the reference used t0 + i/hz; input.csv rounds t to 1e-4)
    const double now = static_cast<double>(row) / hz;
    const bool is_check = fx.checks[next].row == row;
    // at a check row feed the unrounded golden state, exactly as the reference did
    const birdie::SimState s = is_check ? state_from_json(fx.checks[next].golden.at("state")) : state_from_row(fx, row);
    model.update(s, now);
    while (next < fx.checks.size() && fx.checks[next].row == row) {
      const auto& ck = fx.checks[next];
      const json& gold = ck.golden;
      // inputs
      const birdie::SimState csv = state_from_row(fx, row);
      double worst_in = 0;
      for (const auto& v : birdie::kVars) worst_in = std::max(worst_in, std::abs(csv.*v.field - s.*v.field));
      const int before = t.failures;
      std::printf("  [%03d] t=%8.4f agl=%7.1f ft", ck.index, ck.t, s.agl_ft);
      if (worst_in > kStateTol) t.fail("input row differs from the golden state by %g", worst_in);
      // geometry
      const auto g = model.geometry(s, W, H, runway ? &*runway : nullptr);
      const auto flare = birdie::flare_cue(model, s, runway ? &*runway : nullptr, set, now);
      double worst_px = 0;
      std::printf("  bird=(%7.1f,%6.1f)  flare=%s\n", g.bird ? g.bird->x : -1.0, g.bird ? g.bird->y : -1.0,
                  flare ? std::to_string(flare->n).c_str() : "-");
      compare_geometry(gold.at("geometry"), g, model, flare, tol_px, t, worst_px);
      std::printf("        geometry %s (worst %.2e px)\n", t.failures == before ? "ok" : "FAILED", worst_px);
      // HUD scene -> renderer command list
      const int before_cmd = t.failures;
      const double ox = meta.at("box_px")[0].get<double>(), oy = meta.at("box_px")[1].get<double>();
      const auto scene = birdie::hud::build_scene(model, s, W, H, ox, oy, set, runway ? &*runway : nullptr, L"", now);
      const auto cl = birdie::hud::commands(scene, set.opacity);
      double worst_cmd = 0;
      compare_commands(gold.at("commands"), widen_utf8(gold.at("text").get<std::string>()), cl, tol_cmd, t, worst_cmd);
      std::printf("        commands %s (%zu floats, %zu chars, worst %.2e)\n", t.failures == before_cmd ? "ok" : "FAILED",
                  cl.cmds.size(), cl.text.size(), worst_cmd);
      // off-screen render vs golden PNG
      compare_image(fx, ck, cl, meta.at("box_px")[2].get<int>(), meta.at("box_px")[3].get<int>(), t);
      check_border(fx, ck, model, s, set, runway ? &*runway : nullptr, W, H, ox, oy, meta.at("box_px")[2].get<int>(),
                   meta.at("box_px")[3].get<int>(), now, t);
      check_tint(model, s, set, runway ? &*runway : nullptr, W, H, ox, oy, meta.at("box_px")[2].get<int>(),
                 meta.at("box_px")[3].get<int>(), now, t);
      ++t.checks;
      ++next;
    }
  }
  if (next != fx.checks.size()) t.fail("%zu checks never reached", fx.checks.size() - next);
  return t.failures;
}

}  // namespace

int main(int argc, char** argv) {
  // The reference converts lat/lon on a sphere and takes ChasePlane's a_x as the eye's lateral offset; the product
  // does neither (core/geo.h). Compare like with like.
  birdie::set_reference_mode(true);
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path(BIRDIE_FIXTURES_DIR);
  std::printf("Birdie %s parity tests, fixtures in %s\n", birdie::version(), root.string().c_str());

  const auto dirs = birdie::test::find_fixtures(root);
  if (dirs.empty()) {
    std::printf("FAIL: no fixtures found\n");
    return 1;
  }
  int failures = 0;
  for (const auto& dir : dirs) {
    try {
      const Fixture fx = birdie::test::load_fixture(dir);
      std::printf("%s: %zu input rows (%.1f s), %zu columns, %zu checks\n", fx.name.c_str(), fx.rows.size(),
                  fx.rows.back()[0] - fx.rows.front()[0], fx.columns.size(), fx.checks.size());
      if (fx.checks.empty()) {
        std::printf("  FAIL: no checks\n");
        ++failures;
      }
      failures += run_fixture(fx);
    } catch (const std::exception& e) {
      std::printf("FAIL: %s\n", e.what());
      ++failures;
    }
  }
  std::printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
  return failures ? 1 : 0;
}
