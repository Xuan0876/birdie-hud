// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// birdie_shot: draws the HUD of a fixture frame with any settings (and any point of an opening / closing) into a PNG
// with straight alpha, for looking at styles and layouts without the sim (tools/hud_shots.py lays them over the
// recorded frame).
//   birdie_shot <fixture_dir> <check_index> <out.png> [key=value ...] [anim=<kind> p=<0..1>] [preset=<name>]
// Values are JSON when they parse as JSON (numbers, true / false), strings otherwise. The product's own settings
// rules apply (settings_from_json): out-of-range values are clamped.
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>

#include "core/camera.h"
#include "core/geo.h"
#include "core/model.h"
#include "core/runway.h"
#include "core/settings.h"
#include "core/state.h"
#include "fixture.h"
#include "hud/anim.h"
#include "hud/scene.h"
#include "png.h"
#include "render/d2d.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

double num(const json& j) { return j.is_string() ? std::stod(j.get<std::string>()) : j.get<double>(); }

birdie::SimState state_from_row(const birdie::test::Fixture& fx, size_t row) {
  birdie::SimState s;
  for (const auto& v : birdie::kVars)
    if (!birdie::not_in_reference(v.key)) s.*v.field = fx.at(row, v.key);
  s.ra_raw_ft = fx.at(row, "ra_raw_ft");
  birdie::camera_from_chaseplane(s);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::printf("usage: birdie_shot <fixture_dir> <check_index> <out.png> [key=value ...] [anim=kind p=0..1]\n");
    return 2;
  }
  try {
    const auto fx = birdie::test::load_fixture(argv[1]);
    const int index = std::stoi(argv[2]);
    const json& meta = fx.meta;
    json sj = meta.at("settings");
    std::string anim = "none", preset;
  std::wstring wm;  // wm=zh / wm=en: the test-build watermark (app/i18n.cpp S::Watermark)
    double p = 1.0;
    std::optional<double> bank;  // bank=<deg>: every row flown at this bank (a look at the roll scale)
    bool ground = false;         // ground=1: the last row on the ground (a look at the ground-only symbols)
    for (int i = 4; i < argc; ++i) {
      const std::string a = argv[i];
      const auto eq = a.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
      if (k == "anim") anim = v;
      else if (k == "p") p = std::stod(v);
      else if (k == "preset") preset = v;
      else if (k == "bank") bank = std::stod(v);
      else if (k == "ground") ground = v == "1";
      else if (k == "wm") wm = v == "zh" ? L"Birdie 测试版" : L"Birdie beta";
      else {
        json jv;
        try {
          jv = json::parse(v);
        } catch (const std::exception&) {
          jv = v;
        }
        sj[k] = jv;
      }
    }
    birdie::Settings set = birdie::settings_from_json(sj);
    if (!preset.empty()) {
      const json keep = sj;
      birdie::apply_preset(set, preset);
      // explicit key=value after the preset still wins
      for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        const auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = a.substr(0, eq);
        if (k != "anim" && k != "p" && k != "preset" && k != "wm" && k != "bank" && k != "ground") birdie::set_look_value(set, k, keep[k]);
      }
    }
    const auto runway = [&]() -> std::optional<birdie::Runway> {
      if (!meta.contains("runway") || meta["runway"].is_null()) return std::nullopt;
      const json& r = meta["runway"];
      return birdie::make_runway("TEST", num(r.at("lat")), num(r.at("lon")), num(r.at("alt_m")),
                                 num(r.at("landing_hdg_true")), num(r.at("length_m")), num(r.at("width_m")),
                                 r.at("name").get<std::string>());
    }();
    const double W = meta.at("client_px")[0].get<double>(), H = meta.at("client_px")[1].get<double>();
    const double hz = meta.at("input").value("hz", 30.0);
    const double ox = meta.at("box_px")[0].get<double>(), oy = meta.at("box_px")[1].get<double>();
    const int bw = meta.at("box_px")[2].get<int>(), bh = meta.at("box_px")[3].get<int>();
    const auto& ck = fx.checks.at(index);
    birdie::Model model(set);
    birdie::SimState s;
    for (size_t row = 0; row <= ck.row; ++row) {
      s = state_from_row(fx, row);
      if (bank) s.bank_deg = *bank;
      if (ground && row == ck.row) s.on_ground = 1;
      model.update(s, static_cast<double>(row) / hz);
    }
    const double now = static_cast<double>(ck.row) / hz;
    const auto kind = birdie::hud::anim_kind(anim);
    const birdie::Settings fs_ = birdie::hud::anim_settings(set, kind, p);
    const auto scene = birdie::hud::build_scene(model, s, W, H, ox, oy, fs_, runway ? &*runway : nullptr, L"", now,
                                                nullptr, wm);
    const auto cl = birdie::hud::animated_commands(scene, set.opacity, kind, p, now);
    birdie::render::D2DRenderer r;
    if (!r.init_offscreen()) {
      std::printf("renderer init failed\n");
      return 1;
    }
    r.set_font_family(birdie::hud::font_family(set.hud_font));
    std::vector<std::uint8_t> bgra;
    if (!r.render_to_memory(cl, bw, bh, bgra)) {
      std::printf("render failed\n");
      return 1;
    }
    birdie::test::Image img{bw, bh, std::vector<std::uint8_t>(bgra.size())};
    for (size_t i = 0; i < bgra.size(); i += 4) {  // premultiplied BGRA -> straight RGBA
      const int a = bgra[i + 3];
      auto un = [&](int v) { return static_cast<std::uint8_t>(a ? std::min(255, v * 255 / a) : 0); };
      img.px[i] = un(bgra[i + 2]);
      img.px[i + 1] = un(bgra[i + 1]);
      img.px[i + 2] = un(bgra[i]);
      img.px[i + 3] = static_cast<std::uint8_t>(a);
    }
    if (!birdie::test::save_png_rgba(argv[3], img)) {
      std::printf("cannot write %s\n", argv[3]);
      return 1;
    }
    std::printf("%s %dx%d\n", argv[3], bw, bh);
    return 0;
  } catch (const std::exception& e) {
    std::printf("error: %s\n", e.what());
    return 1;
  }
}
