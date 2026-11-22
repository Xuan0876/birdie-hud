// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Small unit tests for what the parity fixtures do not exercise (runway records and selection, settings JSON).
#include <windows.h>

#include <cmath>
#include <filesystem>
#include <cstdio>
#include <string>

#include "core/border.h"
#include "core/geo.h"
#include "core/model.h"
#include "core/pause.h"
#include "core/runway.h"
#include "core/runway_heights.h"
#include "core/settings.h"
#include "hud/scene.h"

namespace {

int g_fail = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      ++g_fail;                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                  \
  } while (0)

void test_names() {
  CHECK(birdie::runway_name(7, 1) == "07L");
  CHECK(birdie::runway_name(25, 2) == "25R");
  CHECK(birdie::runway_name(36, 0) == "36");
  CHECK(birdie::runway_name(0, 0) == "#0");
}

birdie::RunwayRecord lax07l() {
  // LAX 07L/25R centre (OpenStreetMap), heading 82.97 true, 3388 x 45 m, slope +0.1° along 07L
  return {33.9376997, -118.4011429, 26.6, 82.97f, 3388.f, 45.f, 7, 1, 25, 2, 0.1f};
}

void test_ends() {
  const auto ends = birdie::ends_from_record("KLAX", lax07l());
  CHECK(ends.size() == 2);
  if (ends.size() != 2) return;
  CHECK(ends[0].name == "07L" && ends[1].name == "25R");
  CHECK(std::abs(ends[1].hdg - std::fmod(ends[0].hdg + 180, 360)) < 1e-9);
  CHECK(std::abs(ends[0].slope - 0.1) < 1e-6 && std::abs(ends[1].slope + 0.1) < 1e-6);
  auto bad = lax07l();
  bad.length = 50;  // implausible
  CHECK(birdie::ends_from_record("KLAX", bad).empty());
  bad = lax07l();
  bad.true_slope = NAN;
  const auto e2 = birdie::ends_from_record("KLAX", bad);
  CHECK(e2.size() == 2 && e2[0].slope == 0.0);
}

void test_slope_corners() {
  // 07L uphill by 0.1°: near corners 3388/2 * tan(0.1°) = 2.957 m below the centre, far corners as much above;
  // the opposite end (25R) sees the same pavement from the other side
  const auto ends = birdie::ends_from_record("KLAX", lax07l());
  if (ends.size() != 2) return;
  const double d = 1694.0 * std::tan(birdie::rad(0.1));
  const auto c = ends[0].corners();
  CHECK(std::abs(c[0].alt_m - (26.6 - d)) < 1e-4 && std::abs(c[1].alt_m - (26.6 - d)) < 1e-4);
  CHECK(std::abs(c[2].alt_m - (26.6 + d)) < 1e-4 && std::abs(c[3].alt_m - (26.6 + d)) < 1e-4);
  const auto o = ends[1].corners();  // 25R near end = 07L far end
  CHECK(std::abs(o[0].alt_m - (26.6 + d)) < 1e-4 && std::abs(o[2].alt_m - (26.6 - d)) < 1e-4);
  CHECK(std::abs(o[0].lat - c[2].lat) < 1e-9 || std::abs(o[0].lat - c[3].lat) < 1e-9);
  // flat runway: every corner at the centre altitude (the reference behaviour)
  auto flat = lax07l();
  flat.true_slope = 0.f;
  for (const auto& k : birdie::ends_from_record("KLAX", flat)[0].corners()) CHECK(k.alt_m == 26.6);
}

void test_anchor() {
  // KASE 15/33 as MSFS 2024 reports it: ALTITUDE 2340.86 m is the 15 end, not the centre. Measured 11/15 with the
  // A321 lined up on 15, 32 m before the pavement and then on it: ground 2340.28 m.
  const birdie::RunwayRecord kase{39.221895, -106.868239, 2340.86, 159.66f, 2440.f, 33.f, 15, 0, 33, 0, 1.107f};
  auto ends = birdie::ends_from_record("KASE", kase);
  if (ends.size() != 2) return;
  const double h = birdie::rad(159.66);
  const auto at = [&](double along) {
    return birdie::offset_latlon(kase.lat, kase.lon, along * std::sin(h), along * std::cos(h));
  };
  const auto off = at(-1252);  // before the 15 pavement: not anchored
  CHECK(!birdie::anchor_from_ground(ends[0], off.x, off.y, 2340.28) && !ends[0].anchored);
  const auto on = at(-1200);  // on the pavement, 20 m in
  CHECK(birdie::anchor_from_ground(ends[0], on.x, on.y, 2340.28) && ends[0].anchored);
  CHECK(birdie::anchor_from_ground(ends[1], on.x, on.y, 2340.28));
  const double rise = 1220 * std::tan(birdie::rad(1.107));  // 23.6 m from the end to the centre
  CHECK(std::abs(ends[0].alt_m - (2340.28 + 1200 * std::tan(birdie::rad(1.107)))) < 1e-6);
  CHECK(std::abs(ends[0].alt_m - ends[1].alt_m) < 1e-6);  // both directions agree on the same pavement
  CHECK(std::abs(ends[0].corners()[0].alt_m - (ends[0].alt_m - rise)) < 1e-6);
  CHECK(std::abs(ends[0].facility_alt_m - 2340.86) < 1e-9);
  // 40 m to the side: off the 33 m pavement
  const auto side = birdie::offset_latlon(on.x, on.y, 40 * std::cos(h), -40 * std::sin(h));
  CHECK(!birdie::anchor_from_ground(ends[0], side.x, side.y, 2000));
}

void test_pick() {
  const auto ends = birdie::ends_from_record("KLAX", lax07l());
  std::vector<const birdie::Runway*> ptrs{&ends[0], &ends[1]};
  // 8.5 km west of the centre on the 07L centreline, tracking 083: 07L
  const double h = birdie::rad(82.97);
  const auto pos = birdie::offset_latlon(33.9376997, -118.4011429, -8500 * std::sin(h), -8500 * std::cos(h));
  const birdie::Runway* rw = birdie::pick_runway(ptrs, pos.x, pos.y, 83.0, nullptr);
  CHECK(rw == &ends[0]);
  // same place, tracking 263 (away from the runway): 25R is behind us -> nothing
  CHECK(birdie::pick_runway(ptrs, pos.x, pos.y, 263.0, nullptr) == nullptr);
  // 40° off the runway heading: nothing
  CHECK(birdie::pick_runway(ptrs, pos.x, pos.y, 123.0, nullptr) == nullptr);
  // 4 km to the side of the centreline: nothing
  const auto side = birdie::offset_latlon(pos.x, pos.y, 4000 * std::cos(h), -4000 * std::sin(h));
  CHECK(birdie::pick_runway(ptrs, side.x, side.y, 83.0, nullptr) == nullptr);
  // current stays while it is the best
  CHECK(birdie::pick_runway(ptrs, pos.x, pos.y, 83.0, &ends[0]) == &ends[0]);
  // a parallel runway 1 km to the right ("07R"): lined up with it, it takes over from the current 07L (ZULS 10L/10R)
  birdie::Runway r = ends[0];
  r.name = "07R";
  const auto rc = birdie::offset_latlon(r.lat, r.lon, 1000 * std::cos(h), -1000 * std::sin(h));
  r.lat = rc.x, r.lon = rc.y;
  std::vector<const birdie::Runway*> two{&ends[0], &r};
  const auto on_r = birdie::offset_latlon(pos.x, pos.y, 1000 * std::cos(h), -1000 * std::sin(h));
  CHECK(birdie::pick_runway(two, on_r.x, on_r.y, 83.0, &ends[0]) == &r);
  // half-way between the two (a few metres either way): whichever was picked stays
  const auto mid = birdie::offset_latlon(pos.x, pos.y, 530 * std::cos(h), -530 * std::sin(h));
  CHECK(birdie::pick_runway(two, mid.x, mid.y, 83.0, &ends[0]) == &ends[0]);
  CHECK(birdie::pick_runway(two, mid.x, mid.y, 83.0, &r) == &r);
}

// Runway heights before touchdown (core/runway_heights.h): two parallel runways, facility heights 12 / 17 m too high.
void test_heights() {
  using namespace birdie;
  std::vector<Runway> rw;
  for (auto& e : ends_from_record("ZULS", {29.30, 90.90, 3581.7, 89.0f, 4000, 50, 10, 1, 28, 2, 0.0f})) rw.push_back(e);
  for (auto& e : ends_from_record("ZULS", {29.29, 90.90, 3585.5, 89.0f, 4000, 50, 10, 2, 28, 1, 0.0f})) rw.push_back(e);
  std::vector<Runway*> ap;
  for (auto& r : rw) ap.push_back(&r);
  CHECK(rw.size() == 4 && rw[0].name == "10L" && rw[2].name == "10R" && rw[0].alt_src == kAltFacility);
  RunwayHeights none;
  // nothing measured, no DEM: as MSFS says
  CHECK(refine_heights(ap, none, {}).empty() && rw[2].alt_m == 3585.5);
  // the DEM says 3569 everywhere: taken
  DemFn flat = [](double, double) -> std::optional<double> { return 3569.0; };
  CHECK(refine_heights(ap, none, flat).size() == 4 && rw[2].alt_src == kAltDem && std::abs(rw[2].alt_m - 3569) < 1e-9);
  // over 10R's pavement: measured 3568.2; the DEM was 0.8 m off there, so 10L = DEM - 0.8 (better than borrowing
  // 10R's -17.3 m offset, which would put 10L at 3564.4)
  CHECK(anchor_from_ground(rw[2], rw[2].lat, rw[2].lon, 3568.2) && anchor_from_ground(rw[3], rw[3].lat, rw[3].lon, 3568.2));
  refine_heights(ap, none, flat);
  CHECK(rw[2].alt_src == kAltGround && rw[0].alt_src == kAltAirport && std::abs(rw[0].alt_m - 3568.2) < 1e-6);
  // a flattened third-party airport: the DEM 9 m off at the measured runway -> the offset is borrowed instead
  std::vector<Runway> rw2 = rw;
  std::vector<Runway*> ap2;
  for (auto& r : rw2) ap2.push_back(&r), r.alt_src = r.anchored ? kAltGround : kAltFacility;
  DemFn hilly = [](double, double) -> std::optional<double> { return 3577.2; };
  refine_heights(ap2, none, hilly);
  CHECK(rw2[0].alt_src == kAltAirport && std::abs(rw2[0].alt_m - (3581.7 + (3568.2 - 3585.5))) < 1e-6);
  // remembered for the next flight, only while the facility height is the same
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const auto file = std::filesystem::path(tmp) / L"birdie_test_runway_heights.json";
  std::filesystem::remove(file);
  {
    RunwayHeights h(file);
    CHECK(h.put(rw[2]) && !h.put(rw[2]) && h.save());
  }
  RunwayHeights h(file);
  std::vector<Runway> fresh = ends_from_record("ZULS", {29.29, 90.90, 3585.5, 89.0f, 4000, 50, 10, 2, 28, 1, 0.0f});
  CHECK(h.size() == 1 && h.get(fresh[0]) && std::abs(*h.get(fresh[0]) - 3568.2) < 0.06 && !h.get(fresh[1]));
  std::vector<Runway*> ap3{&fresh[0], &fresh[1]};
  refine_heights(ap3, h, flat);
  CHECK(fresh[0].alt_src == kAltSaved && fresh[1].alt_src == kAltAirport);
  fresh[0].facility_alt_m = 3590;  // other scenery: the measurement no longer counts
  CHECK(!h.get(fresh[0]));
  std::filesystem::remove(file);
}

void test_settings() {
  auto j = nlohmann::json::parse(R"({"glide_deg": 3.5, "show_bird": false, "lead_ms": 40, "unknown": 1,
                                      "hud_scale": "bad", "ra_offset_ft": 14.5,
                                      "ra_by_aircraft": {"FSLabs A321": {"ft": 14.5, "note": "x"},
                                                         "bad": {"ft": 500}, "worse": 3}})");
  const auto s = birdie::settings_from_json(j);
  CHECK(s.glide_deg == 3.5 && !s.show_bird && s.lead_ms == 40 && s.hud_scale == 1.0);
  CHECK(s.ra_by_aircraft.size() == 1 && s.ra_by_aircraft.at("FSLabs A321").ft == 14.5 &&
        s.ra_by_aircraft.at("FSLabs A321").note == "x");
  const auto back = birdie::settings_from_json(birdie::settings_to_json(s));
  CHECK(back.glide_deg == 3.5 && !back.show_bird && back.lead_ms == 40 && back.ra_by_aircraft.size() == 1);
}

void test_focal() {
  // measured 11/15 (tools/zoom_calib): zoom -> focal px at 1440 px height
  const double meas[][2] = {{0.116, 590.4}, {0.351, 944.3}, {0.630, 1828.8}, {0.734, 2571.1}, {0.828, 3952.6}};
  for (const auto& m : meas) CHECK(std::abs(birdie::focal_px(m[0], 1440) / m[1] - 1) < 0.005);
  CHECK(std::abs(birdie::focal_px(0.49725, 1440) - 1298.0) < 1e-6);  // the video-verified anchor
  CHECK(std::abs(birdie::focal_px(0.5, 1600) / birdie::focal_px(0.5, 1440) - 1600.0 / 1440) < 1e-12);  // height
}

void test_ground_ra() {
  std::vector<double> v(60, 14.5);
  v[3] = 14.3;
  const auto r = birdie::steady_ground_ra(v);
  CHECK(r && std::abs(*r - 14.5) < 1e-9);
  CHECK(!birdie::steady_ground_ra(std::vector<double>(59, 14.5)));  // too few
  v[7] = 15.2;                                                       // not steady
  CHECK(!birdie::steady_ground_ra(v));
  CHECK(!birdie::steady_ground_ra(std::vector<double>(60, 2500.0)));  // implausible
}

void test_hide() {
  birdie::SimState s;
  s.cp_zoom = 0.5;
  s.gs_kt = 150;
  CHECK(birdie::hide_reason(s) == nullptr);
  s.cp_yaw = 60;
  CHECK(std::string(birdie::hide_reason(s)) == "not_forward");
  s.cp_yaw = 0;
  s.cp_mode = 1;
  CHECK(std::string(birdie::hide_reason(s)) == "external_view");
  s.cp_mode = 0;
  s.cp_zoom = 0;
  CHECK(std::string(birdie::hide_reason(s)) == "no_chaseplane");
  s.cp_zoom = 0.5;
  s.on_ground = 1;
  s.gs_kt = 10;
  CHECK(std::string(birdie::hide_reason(s)) == "ground_slow");
  s.gs_kt = 45;
  CHECK(birdie::hide_reason(s) == nullptr);                                   // the default: shown above 40 kt
  CHECK(std::string(birdie::hide_reason(s, 60)) == "ground_slow");            // auto_show_kt = 60
  s.gs_kt = 0;
  CHECK(birdie::hide_reason(s, 0) == nullptr);                                // auto_show off: shown standing still
}

}  // namespace

void test_wgs84() {
  // local metres per degree on WGS84 at 45°: M·π/180 = 111131.78 north, N·cos·π/180 = 78846.84 east
  // (the often quoted 111132.95 is the arc of a whole degree, 44.5°..45.5°)
  const auto en = birdie::enu(45.0, 7.0, 45.001, 7.001);
  CHECK(std::abs(en.y / 0.001 - 111131.78) < 0.02);
  CHECK(std::abs(en.x / 0.001 - 78846.84) < 0.02);
  // round trip
  const auto ll = birdie::offset_latlon(29.8268, 121.4619, -3000.0, 2000.0);
  const auto back = birdie::enu(29.8268, 121.4619, ll.x, ll.y);
  CHECK(std::abs(back.x + 3000.0) < 1e-6 && std::abs(back.y - 2000.0) < 1e-6);
  // the sphere (Python reference, parity tests only) is still there
  birdie::set_reference_mode(true);
  CHECK(std::abs(birdie::enu(45.0, 7.0, 45.001, 7.0).y / 0.001 - birdie::rad(1.0) * birdie::kEarthRadius) < 1e-6);
  CHECK(birdie::eye_lateral_m(0.548, -0.019) == 0.548);
  birdie::set_reference_mode(false);
}

void test_eye() {
  // FBW A320 11/15: pilot a_x 0.548 / r_x -0.0195, copilot a_x 1.737 / r_x 1.170 -> symmetric seats
  CHECK(std::abs(birdie::eye_lateral_m(0.548, -0.0195) + 0.587) < 1e-9);
  CHECK(std::abs(birdie::eye_lateral_m(1.737, 1.170) - 0.603) < 1e-9);
  CHECK(birdie::eye_lateral_m(0.0, 0.0) == 0.0);  // no ChasePlane
}

void test_pause() {
  // Pause_EX1 flags -> hide the HUD (the ESC menu sends data while paused: only these flags tell)
  using birdie::pause_hides_hud;
  CHECK(!pause_hides_hud(0x0));                       // running
  CHECK(pause_hides_hud(birdie::kPauseFull));         // pause key / ESC menu
  CHECK(pause_hides_hud(birdie::kPauseLegacy));       // FSX pause with sound
  CHECK(pause_hides_hud(birdie::kPauseSim));          // user aircraft paused, traffic running
  CHECK(pause_hides_hud(birdie::kPauseActive, true));   // Active Pause: hides when chosen
  CHECK(!pause_hides_hud(birdie::kPauseActive, false));  // ... and stays when not
  CHECK(pause_hides_hud(birdie::kPauseActive | birdie::kPauseSim, false));  // another pause on top still hides
  CHECK(pause_hides_hud(0x10, false));                // an unknown bit is still a pause
  CHECK(pause_hides_hud(birdie::kPauseActive) == birdie::kHideOnActivePause);  // the default
}

void test_border() {
  using namespace birdie;
  CHECK(seat_of(-0.59) == Seat::kLeft && seat_of(0.60) == Seat::kRight);
  Settings::BorderPoly b;
  b.pts = {{-10, 5}, {12, 6}, {11, -8}, {-9, -7}};
  b.eye = {-0.59, 1.0, 2.0};
  const auto m = mirrored(b);
  CHECK(m.pts.size() == 4 && m.pts[0].x == 9 && m.pts[0].y == -7 && m.pts[3].x == 10 && m.pts[3].y == 5);
  CHECK(m.eye.x == 0.59 && m.eye.y == 1.0);
  const auto mm = mirrored(m);
  bool same = mm.pts.size() == b.pts.size();
  for (size_t i = 0; same && i < b.pts.size(); ++i) same = mm.pts[i] == b.pts[i];
  CHECK(same);
  Settings::BorderSeats seats;
  seats.left = b;
  std::string how;
  CHECK(border_for(seats, Seat::kLeft, &how)->pts[1].x == 12 && how == "drawn");
  CHECK(border_for(seats, Seat::kRight, &how)->pts[0].x == 9 && how == "mirrored");
  CHECK(!border_for(Settings::BorderSeats{}, Seat::kLeft, &how) && how == "none");
  // vertices: clamped to +-60, non-finite dropped, 3..32 of them
  std::vector<Vec2> bad{{0, 0}, {100, 0}, {std::nan(""), 1}};
  CHECK(!sanitize_border(bad));
  std::vector<Vec2> ok{{0, 0}, {100, 0}, {0, -100}};
  CHECK(sanitize_border(ok) && ok[1].x == 60 && ok[2].y == -60);
  std::vector<Vec2> many(33, Vec2{1, 1});
  CHECK(!sanitize_border(many));
  // inside / distance
  const auto canvas = default_border();
  CHECK(inside_polygon(canvas, {0, 0}) && !inside_polygon(canvas, {0, 10}) && !inside_polygon(canvas, {18, 0}));
  CHECK(std::abs(distance_to_edges(canvas, {0, 8}) - 1.0) < 1e-12);
  // fit: the instruments shrink about the boresight until they clear the edges by the margin
  const std::vector<Vec2> box15{{-15, 15}, {15, 15}, {15, -15}, {-15, -15}}, box10{{-10, 10}, {10, 10}, {10, -10}, {-10, -10}};
  const std::vector<Vec2> pts{{20, 0}, {-20, 0}, {0, 20}};
  const auto k = fit_scale(box15, pts, 0.8, 0.6, 1.0);
  CHECK(k && std::abs(*k - 0.71) < 1e-9);  // (15 - 0.8) / 20 = 0.71
  CHECK(!fit_scale(box10, pts, 0.8, 0.6, 1.0));  // would need 0.46
  CHECK(fit_scale(box15, {{1, 1}}, 0.8, 0.6, 1.0) == 1.0);
  CHECK(all_inside(box15, {{14, 0}}, 0.8) && !all_inside(box15, {{14.5, 0}}, 0.8));
  // settings.json: per aircraft and seat; absent until the first border; malformed entries skipped
  Settings s;
  s.border_by_aircraft["A321"].left = b;
  const auto back = settings_from_json(settings_to_json(s));
  CHECK(back.border_by_aircraft.size() == 1 && back.border_by_aircraft.at("A321").left &&
        !back.border_by_aircraft.at("A321").right);
  if (back.border_by_aircraft.count("A321") && back.border_by_aircraft.at("A321").left) {
    const auto& r = *back.border_by_aircraft.at("A321").left;
    CHECK(r.pts.size() == 4 && r.pts[2].x == 11 && r.pts[2].y == -8 && r.eye.z == 2.0);
  }
  CHECK(!settings_to_json(Settings{}).contains("border_by_aircraft"));
  const auto j2 = nlohmann::json::parse(R"({"border_by_aircraft": {"x": {"L": {"pts": [[0,0],[1,1]]}},
      "y": {"R": {"pts": [[0,0],[1,"a"],[2,2]]}}, "z": 5, "w": {"L": {"pts": [[0,0],[5,0],[0,5]]}}}})");
  const auto s2 = settings_from_json(j2);
  CHECK(s2.border_by_aircraft.size() == 1 && s2.border_by_aircraft.count("w") && s2.border_by_aircraft.at("w").left);
}

void test_border_projection() {
  using namespace birdie;
  // plane degrees -> screen -> plane, with the head turned and the aircraft pitched and banked
  Settings set;
  Model model(set);
  SimState s;
  s.cp_zoom = 0.5, s.cp_yaw = 20, s.cp_pitch = -8, s.cp_roll = 3, s.pitch_deg = -4, s.bank_deg = 10, s.hdg_true = 70;
  const double W = 3440, H = 1440, ox = 100, oy = 50;
  const std::vector<Vec2> pts{{-12, 7}, {14, 8}, {15, -14}, {-13, -15}};
  const auto v = hud::border_vertices(model, s, W, H, ox, oy, pts);
  CHECK(v.size() == pts.size());
  for (size_t i = 0; i < v.size() && i < pts.size(); ++i) {
    CHECK(v[i].has_value());
    if (!v[i]) continue;
    const auto q = hud::screen_to_border(model, s, W, H, ox, oy, *v[i]);
    CHECK(q && std::abs(q->x - pts[i].x) < 1e-9 && std::abs(q->y - pts[i].y) < 1e-9);
  }
  // the default border (the canvas at size 1) lands on the HUD's own field-of-view clip
  const auto canvas = default_border();
  const auto sc = hud::build_scene(model, s, W, H, ox, oy, set, nullptr, L"", 0.0, &canvas);
  CHECK(sc.clip && sc.border && sc.border->first.size() == 4);
  if (sc.clip && sc.border && sc.border->first.size() == 4)
    for (size_t i = 0; i < 4; ++i)
      CHECK(std::abs((*sc.clip)[i].x - sc.border->first[i].x) < 0.01 && std::abs((*sc.clip)[i].y - sc.border->first[i].y) < 0.01);
  if (sc.border) CHECK(std::abs(sc.border->second - focal_px(0.5, H) * std::tan(rad(1.0))) < 0.05);
  // no border: no border command at all
  const auto plain = hud::build_scene(model, s, W, H, ox, oy, set, nullptr, L"", 0.0);
  CHECK(!plain.border);
  // head turned far: the part behind the camera is cut off, the rest still clips
  s.cp_yaw = 80;
  const std::vector<Vec2> wide{{-60, 30}, {60, 30}, {60, -30}, {-60, -30}};
  const auto cut = hud::border_to_screen(model, s, W, H, ox, oy, wide);
  CHECK(cut && cut->size() >= 3);
}

int main() {
  test_wgs84();
  test_eye();
  test_names();
  test_ends();
  test_slope_corners();
  test_anchor();
  test_pick();
  test_heights();
  test_settings();
  test_ground_ra();
  test_focal();
  test_hide();
  test_pause();
  test_border();
  test_border_projection();
  std::printf(g_fail ? "unit tests FAILED (%d)\n" : "unit tests PASSED\n", g_fail);
  return g_fail ? 1 : 0;
}
