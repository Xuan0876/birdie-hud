// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Terrain without the network: the GeoTIFF reader on a file made here (DEFLATE + floating-point predictor, like the
// Copernicus tiles), the land list, the tile sizes, and the skyline on a made-up landscape.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/geo.h"
#include "core/settings.h"
#include "miniz.h"
#include "terrain/geotiff.h"
#include "terrain/store.h"
#include "terrain/terrain.h"

namespace {

int g_fail = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      ++g_fail;                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                  \
  } while (0)

namespace fs = std::filesystem;
using namespace birdie::terrain;

fs::path temp_dir(const char* name) {
  wchar_t t[MAX_PATH];
  GetTempPathW(MAX_PATH, t);
  fs::path p = fs::path(t) / (std::string("birdie_test_") + name);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p);
  return p;
}

// A little-endian TIFF: W x H float32 heights f(x, y), 16 px tiles, DEFLATE, predictor 3, PixelIsPoint at (lat0, lon0).
std::vector<uint8_t> make_tif(int W, int H, double lat0, double lon0, double step, float (*f)(int, int)) {
  const int T = 16, across = (W + T - 1) / T, down = (H + T - 1) / T;
  std::vector<std::vector<uint8_t>> tiles;
  for (int ty = 0; ty < down; ++ty)
    for (int tx = 0; tx < across; ++tx) {
      std::vector<uint8_t> raw(static_cast<size_t>(T) * T * 4);
      for (int y = 0; y < T; ++y) {
        uint8_t* row = raw.data() + static_cast<size_t>(y) * T * 4;
        for (int x = 0; x < T; ++x) {
          const int gx = tx * T + x, gy = ty * T + y;
          const float v = gx < W && gy < H ? f(gx, gy) : 0.0f;
          uint8_t b[4];
          std::memcpy(b, &v, 4);
          for (int k = 0; k < 4; ++k) row[k * T + x] = b[3 - k];  // byte planes, most significant first
        }
        for (int i = T * 4 - 1; i > 0; --i) row[i] = static_cast<uint8_t>(row[i] - row[i - 1]);
      }
      mz_ulong n = mz_compressBound(static_cast<mz_ulong>(raw.size()));
      std::vector<uint8_t> z(n);
      mz_compress(z.data(), &n, raw.data(), static_cast<mz_ulong>(raw.size()));
      z.resize(n);
      tiles.push_back(std::move(z));
    }
  std::vector<uint8_t> out(8);
  auto put = [&](size_t at, const void* p, size_t k) {
    if (out.size() < at + k) out.resize(at + k);
    std::memcpy(out.data() + at, p, k);
  };
  out[0] = out[1] = 'I';
  const uint16_t magic = 42;
  put(2, &magic, 2);
  // arrays first, then the directory
  std::vector<uint32_t> offs, lens;
  for (const auto& t : tiles) {
    offs.push_back(static_cast<uint32_t>(out.size()));
    lens.push_back(static_cast<uint32_t>(t.size()));
    put(out.size(), t.data(), t.size());
  }
  const uint32_t a_offs = static_cast<uint32_t>(out.size());
  put(out.size(), offs.data(), offs.size() * 4);
  const uint32_t a_lens = static_cast<uint32_t>(out.size());
  put(out.size(), lens.data(), lens.size() * 4);
  const double scale[3] = {step, step, 0}, tie[6] = {0, 0, 0, lon0, lat0, 0};
  const uint32_t a_scale = static_cast<uint32_t>(out.size());
  put(out.size(), scale, sizeof scale);
  const uint32_t a_tie = static_cast<uint32_t>(out.size());
  put(out.size(), tie, sizeof tie);
  const uint16_t keys[8] = {1, 1, 0, 1, 1025, 0, 1, 2};  // GTRasterTypeGeoKey = PixelIsPoint
  const uint32_t a_keys = static_cast<uint32_t>(out.size());
  put(out.size(), keys, sizeof keys);
  struct E {
    uint16_t tag, type;
    uint32_t count, value;
  };
  const uint32_t ntiles = static_cast<uint32_t>(tiles.size());
  const E e[] = {{256, 3, 1, static_cast<uint32_t>(W)}, {257, 3, 1, static_cast<uint32_t>(H)}, {258, 3, 1, 32},
                 {259, 3, 1, 8},  {277, 3, 1, 1},  {317, 3, 1, 3},  {322, 3, 1, static_cast<uint32_t>(T)},
                 {323, 3, 1, static_cast<uint32_t>(T)}, {324, 4, ntiles, ntiles == 1 ? offs[0] : a_offs},
                 {325, 4, ntiles, ntiles == 1 ? lens[0] : a_lens}, {339, 3, 1, 3}, {33550, 12, 3, a_scale},
                 {33922, 12, 6, a_tie}, {34735, 3, 8, a_keys}};
  const uint32_t ifd = static_cast<uint32_t>(out.size());
  put(4, &ifd, 4);
  const uint16_t n = static_cast<uint16_t>(std::size(e));
  put(out.size(), &n, 2);
  for (const auto& x : e) put(out.size(), &x, 12);
  const uint32_t next = 0;
  put(out.size(), &next, 4);
  return out;
}

void test_geotiff() {
  const fs::path dir = temp_dir("geotiff");
  // 40 x 30 samples, 0.0002 deg apart, first sample at 35.0 N / 120.0 E: lands in N34E120
  auto f = [](int x, int y) { return static_cast<float>(100.25 + 3.5 * x - 2.0 * y); };
  const auto tif = make_tif(40, 30, 35.0, 120.0, 0.0002, f);
  BdemInfo info;
  std::string err;
  CHECK(geotiff_to_bdem(tif, dir, &info, &err));
  CHECK(info.name == "N34E120" && info.w == 40 && info.h == 30 && info.res_m == 30);
  std::ifstream in(dir / "N34E120.bdem", std::ios::binary);
  std::vector<char> b((std::istreambuf_iterator<char>(in)), {});
  CHECK(b.size() == 56 + 2 * 40 * 30);
  if (b.size() == 56 + 2 * 40 * 30) {
    int16_t z[3];
    std::memcpy(&z[0], b.data() + 56, 2);                        // (0, 0)
    std::memcpy(&z[1], b.data() + 56 + 2 * (39), 2);             // (39, 0)
    std::memcpy(&z[2], b.data() + 56 + 2 * (29 * 40 + 39), 2);   // (39, 29)
    CHECK(z[0] == 100 && z[1] == static_cast<int16_t>(std::lround(100.25 + 3.5 * 39)) &&
          z[2] == static_cast<int16_t>(std::lround(100.25 + 3.5 * 39 - 2.0 * 29)));
    double lat0;
    std::memcpy(&lat0, b.data() + 16, 8);
    CHECK(lat0 == 35.0);
  }
  CHECK(bdem_res(dir / "N34E120.bdem") == 30);
  // broken input is refused, not crashed on
  std::vector<uint8_t> cut(tif.begin(), tif.begin() + tif.size() / 2);
  CHECK(!geotiff_to_bdem(cut, dir, &info, &err) && !err.empty());
  CHECK(!geotiff_to_bdem({'M', 'M', 0, 42}, dir, &info, &err));
}

void test_tiles() {
  CHECK(tile_name(34, -119) == "N34W119" && tile_name(-1, 0) == "S01E000");
  int la = 0, lo = 0;
  CHECK(parse_tile_name("S46E168", &la, &lo) && la == -46 && lo == 168 && !parse_tile_name("N3XE120", &la, &lo));
  CHECK(is_land(18, 109) && is_land(34, -119) && !is_land(0, -150) && !is_land(30, -40));  // Hainan, LA; Pacific, Atlantic
  CHECK(tile_bytes(18, 30) == 56 + 2ull * 3600 * 3600 && tile_bytes(61, 90) == 56 + 2ull * 1200 * 600);
  const auto r = tiles_around(18.3, 109.4, 60);
  CHECK(!r.empty() && r.front() == "N18E109");
}

// A plain at 0 m with two ridges east of the eye: a near one (8 km) north of the heading and a far, higher one (30 km)
// south of it, overlapping in the middle. The skyline must run across both in one line.
void test_skyline() {
  const fs::path dir = temp_dir("skyline");
  const double lat0 = 10.0, lon0 = 20.0;  // tile N10E020 written by hand, 0.001 deg spacing
  const int W = 1001, H = 1001;
  std::vector<int16_t> z(static_cast<size_t>(W) * H, 0);
  const double eye_lat = 10.5, eye_lon = 20.1;
  for (int r = 0; r < H; ++r)
    for (int c = 0; c < W; ++c) {
      const double lat = lat0 + 1.0 - r * 0.001, lon = lon0 + c * 0.001;
      const birdie::Vec2 en = birdie::enu(eye_lat, eye_lon, lat, lon);
      const double e = en.x, n = en.y;
      double h = 0;
      if (n > -2000 && std::abs(e - 8000) < 600) h = std::max(h, 400 * (1 - std::abs(e - 8000) / 600));
      if (n < 2000 && std::abs(e - 30000) < 1500) h = std::max(h, 2000 * (1 - std::abs(e - 30000) / 1500));
      z[static_cast<size_t>(r) * W + c] = static_cast<int16_t>(h);
    }
  {
    std::ofstream f(dir / "N10E020.bdem", std::ios::binary);
    const char magic[4] = {'B', 'D', 'E', 'M'};
    const uint32_t v[3] = {1, W, H};
    const double g[4] = {lat0 + 1.0, lon0, 0.001, 0.001};
    const uint32_t res[2] = {0, 0};
    f.write(magic, 4);
    f.write(reinterpret_cast<const char*>(v), sizeof v);
    f.write(reinterpret_cast<const char*>(g), sizeof g);
    f.write(reinterpret_cast<const char*>(res), sizeof res);
    f.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size() * 2));
  }
  Dem dem(dir);
  RidgeParams rp;
  rp.d_max = 45000;
  rp.az_half = 20;
  const auto t = extract_ridges(dem, eye_lat, eye_lon, 300.0, 90.0, rp);
  size_t longest = 0;
  for (const auto& l : t.lines) longest = std::max(longest, l.size());
  // the two ridges both lie on the skyline, joined: one line over (nearly) the whole 40 deg fan
  CHECK(t.lines.size() == 1 && longest > 150);
  bool saw_near = false, saw_far = false;
  for (const auto& l : t.lines)
    for (const auto& q : l) {
      const birdie::Vec2 en = birdie::enu(eye_lat, eye_lon, q.lat, q.lon);
      saw_near = saw_near || std::abs(en.x - 8000) < 700;
      saw_far = saw_far || std::abs(en.x - 30000) < 1600;
    }
  CHECK(saw_near && saw_far);
}

// The terrain's own border is kept apart from the HUD's and survives settings.json.
void test_terrain_border_setting() {
  birdie::Settings s;
  birdie::Settings::BorderPoly b;
  b.pts = {{-30, 10}, {30, 10}, {30, -25}, {-30, -25}};
  s.terrain_border_by_aircraft["A321"].left = b;
  const auto r = birdie::settings_from_json(birdie::settings_to_json(s));
  CHECK(r.border_by_aircraft.empty() && r.terrain_border_by_aircraft.count("A321") &&
        r.terrain_border_by_aircraft.at("A321").left && r.terrain_border_by_aircraft.at("A321").left->pts.size() == 4 &&
        !r.terrain_border_by_aircraft.at("A321").right);
  CHECK(!birdie::settings_to_json(birdie::Settings{}).contains("terrain_border_by_aircraft"));
}

}  // namespace

int main() {
  test_terrain_border_setting();
  test_geotiff();
  test_tiles();
  test_skyline();
  if (g_fail) {
    std::printf("terrain tests FAILED (%d)\n", g_fail);
    return 1;
  }
  std::printf("terrain tests passed\n");
  return 0;
}
