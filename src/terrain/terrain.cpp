// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "terrain/terrain.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/geo.h"

namespace birdie::terrain {
namespace {

constexpr double kPi = 3.14159265358979323846;
double rad(double d) { return d * kPi / 180.0; }
double deg(double r) { return r * 180.0 / kPi; }
double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
long long key(int ilat, int ilon) { return static_cast<long long>(ilat + 100) * 1000 + (ilon + 400); }

#pragma pack(push, 1)
struct Header {
  char magic[4];
  unsigned version, w, h;
  double lat0, lon0, dlat, dlon;
  unsigned reserved[2];
};
#pragma pack(pop)
static_assert(sizeof(Header) == 56);

}  // namespace

std::string tile_name(int ilat, int ilon) {
  char b[16];
  std::snprintf(b, sizeof b, "%c%02d%c%03d", ilat >= 0 ? 'N' : 'S', std::abs(ilat), ilon >= 0 ? 'E' : 'W',
                std::abs(ilon));
  return b;
}

namespace {
const unsigned char kLand[180 * 360 / 8] = {
#include "terrain/land_tiles.inc"
};
}  // namespace

bool is_land(int ilat, int ilon) {
  if (ilat < -90 || ilat > 89 || ilon < -180 || ilon > 179) return false;
  const int i = (ilat + 90) * 360 + (ilon + 180);
  return (kLand[i >> 3] >> (i & 7)) & 1;
}

bool parse_tile_name(const std::string& n, int* ilat, int* ilon) {
  if (n.size() != 7 || (n[0] != 'N' && n[0] != 'S') || (n[3] != 'E' && n[3] != 'W')) return false;
  for (int i : {1, 2, 4, 5, 6})
    if (n[i] < '0' || n[i] > '9') return false;
  *ilat = std::atoi(n.substr(1, 2).c_str()) * (n[0] == 'S' ? -1 : 1);
  *ilon = std::atoi(n.substr(4, 3).c_str()) * (n[3] == 'W' ? -1 : 1);
  return true;
}

Dem::Dem(std::filesystem::path dir) : dir_(std::move(dir)) {}

Dem::~Dem() {
  for (auto& [k, t] : tiles_) {
    if (t.base) UnmapViewOfFile(t.base);
    if (t.map) CloseHandle(t.map);
    if (t.file) CloseHandle(t.file);
  }
}

const Dem::Tile* Dem::tile(int ilat, int ilon) {
  const long long k = key(ilat, ilon);
  if (const auto it = tiles_.find(k); it != tiles_.end()) return &it->second;
  const double t = now_s();
  if (const auto it = missing_.find(k); it != missing_.end() && t - it->second < 2.0) return nullptr;
  missing_[k] = t;
  const std::string name = tile_name(ilat, ilon);
  std::error_code ec;
  if (std::filesystem::exists(dir_ / (name + ".none"), ec)) {
    Tile s;
    s.sea = true;
    missing_.erase(k);
    return &(tiles_[k] = s);
  }
  // installed packages first, then the streaming cache (FILE_SHARE_DELETE: the cache may drop a file while it is
  // mapped; the mapping stays valid)
  HANDLE f = INVALID_HANDLE_VALUE;
  for (const auto& path : {dir_ / (name + ".bdem"), dir_ / L"cache" / (name + ".bdem")}) {
    f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) break;
  }
  if (f == INVALID_HANDLE_VALUE) {
    if (!is_land(ilat, ilon)) {  // no such Copernicus tile: open sea
      Tile s;
      s.sea = true;
      missing_.erase(k);
      return &(tiles_[k] = s);
    }
    return nullptr;
  }
  LARGE_INTEGER size{};
  GetFileSizeEx(f, &size);
  HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
  const char* base = m ? static_cast<const char*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0)) : nullptr;
  Header hd{};
  if (base && size.QuadPart >= static_cast<long long>(sizeof(Header))) std::memcpy(&hd, base, sizeof hd);
  const bool ok = base && std::memcmp(hd.magic, "BDEM", 4) == 0 && hd.version == 1 && hd.w > 1 && hd.h > 1 &&
                  size.QuadPart >= static_cast<long long>(sizeof(Header) + 2ull * hd.w * hd.h);
  if (!ok) {
    if (base) UnmapViewOfFile(base);
    if (m) CloseHandle(m);
    CloseHandle(f);
    return nullptr;
  }
  Tile tl;
  tl.file = f;
  tl.map = m;
  tl.base = base;
  tl.z = reinterpret_cast<const short*>(base + sizeof(Header));
  tl.w = static_cast<int>(hd.w);
  tl.h = static_cast<int>(hd.h);
  tl.lat0 = hd.lat0;
  tl.lon0 = hd.lon0;
  tl.dlat = hd.dlat;
  tl.dlon = hd.dlon;
  missing_.erase(k);
  return &(tiles_[k] = tl);
}

std::optional<double> Dem::height(double lat, double lon) {
  const int ilat = static_cast<int>(std::floor(lat)), ilon = static_cast<int>(std::floor(lon));
  const Tile* t = tile(ilat, ilon);
  if (!t) return std::nullopt;
  if (t->sea) return 0.0;
  const double r = std::clamp((t->lat0 - lat) / t->dlat, 0.0, t->h - 1.001);
  const double c = std::clamp((lon - t->lon0) / t->dlon, 0.0, t->w - 1.001);
  const int r0 = static_cast<int>(r), c0 = static_cast<int>(c);
  const double fr = r - r0, fc = c - c0;
  auto at = [&](int rr, int cc) {
    const short v = t->z[static_cast<size_t>(rr) * t->w + cc];
    return v == -32768 ? 0.0 : static_cast<double>(v);
  };
  const double a = at(r0, c0) + fc * (at(r0, c0 + 1) - at(r0, c0));
  const double b = at(r0 + 1, c0) + fc * (at(r0 + 1, c0 + 1) - at(r0 + 1, c0));
  return a + fr * (b - a);
}

std::vector<Dem::Near> Dem::around(double lat, double lon, double km) {
  const double dlat = km / 111.0, dlon = km / (111.0 * std::max(0.1, std::cos(rad(lat))));
  std::vector<Near> out;
  for (int la = static_cast<int>(std::floor(lat - dlat)); la <= static_cast<int>(std::floor(lat + dlat)); ++la)
    for (int lo = static_cast<int>(std::floor(lon - dlon)); lo <= static_cast<int>(std::floor(lon + dlon)); ++lo) {
      const int l = ((lo + 180) % 360 + 360) % 360 - 180;
      if (la < -90 || la > 89) continue;
      const double clat = std::clamp(lat, double(la), la + 1.0), clon = std::clamp(lon, double(lo), lo + 1.0);
      const double d = std::hypot((clat - lat) * 111.0, (clon - lon) * 111.0 * std::cos(rad(lat)));
      if (d > km) continue;
      const Tile* t = tile(la, l);
      if (t && t->sea) continue;
      if (!t && !is_land(la, l)) continue;
      out.push_back({tile_name(la, l), d, t != nullptr});
    }
  std::sort(out.begin(), out.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
  return out;
}

void Dem::drop_far(double lat, double lon, double keep_km) {
  for (auto it = tiles_.begin(); it != tiles_.end();) {
    const int la = static_cast<int>(it->first / 1000 - 100), lo = static_cast<int>(it->first % 1000 - 400);
    const double d = std::hypot((la + 0.5 - lat) * 111.0, (lo + 0.5 - lon) * 111.0 * std::cos(rad(lat)));
    if (d > keep_km) {
      Tile& t = it->second;
      if (t.base) UnmapViewOfFile(t.base);
      if (t.map) CloseHandle(t.map);
      if (t.file) CloseHandle(t.file);
      it = tiles_.erase(it);
    } else {
      ++it;
    }
  }
}

TerrainLines extract_ridges(Dem& dem, double lat, double lon, double eye_alt_m, double az_deg, const RidgeParams& rp) {
  TerrainLines out;
  // metres -> degrees near the aircraft (local radii of the ellipsoid, as Birdie's enu())
  const Vec2 p1 = offset_latlon(lat, lon, 0.0, 1000.0), p2 = offset_latlon(lat, lon, 1000.0, 0.0);
  const double dlat_m = (p1.x - lat) / 1000.0, dlon_m = (p2.y - lon) / 1000.0;
  const double R2 = 2.0 * kEarthRadius;
  const double min_drop = std::tan(rad(rp.min_drop_deg));
  const double warn_h = eye_alt_m - rp.warn_below_m;
  struct Sil {
    double d, e, h, la, lo;
    bool sky;
    double drop = 0;  // tan of how far the terrain behind dips below it
  };
  const int n = static_cast<int>(std::floor(2 * rp.az_half / rp.az_step)) + 1;
  std::vector<std::vector<Sil>> cols(n);  // ridges in front of the skyline
  std::vector<std::optional<Sil>> sky(n);  // the skyline: the highest point of each ray
  std::vector<std::optional<Sil>> low(n);  // ... where it does not stand above the sea horizon (to bridge short gaps)
  // A skyline point must stand above the sea horizon seen from here (dip = sqrt(2 h / R)), else it is the horizon
  // of the sea or of a low plain, which is no relief worth a line.
  const double sky_above = std::tan(rad(rp.sky_above_horizon_deg)) - std::sqrt(2.0 * std::max(eye_alt_m, 0.0) / kEarthRadius);
  for (int i = 0; i < n; ++i) {
    const double az = rad(az_deg - rp.az_half + i * rp.az_step);
    const double se = std::sin(az), cn = std::cos(az);
    double emax = -1e9;
    std::optional<Sil> cand;  // last visible point while terrain behind it is hidden
    double hid_min = 1e9;
    Sil last{};
    bool vis_prev = false, have_last = false, cut = false;
    for (double d = rp.d_min; d <= rp.d_max;) {
      const double la = lat + cn * d * dlat_m, lo = lon + se * d * dlon_m;
      const auto h = dem.height(la, lo);
      if (!h) {
        out.skyline_complete = false;
        cut = true;
        break;
      }
      const double e = (*h - eye_alt_m - d * d / R2) / d;  // tan of the elevation angle
      if (e >= emax) {
        if (cand) {  // terrain shows again behind a ridge: keep the ridge if enough lies hidden behind it
          if (cand->e - hid_min >= min_drop) {
            cand->drop = cand->e - hid_min;
            cols[i].push_back(*cand);
          }
          cand.reset();
        }
        emax = e;
        last = {d, e, *h, la, lo, false};
        have_last = true;
        vis_prev = true;
      } else {
        if (vis_prev && have_last) {
          cand = last;
          hid_min = e;
        }
        if (cand) hid_min = std::min(hid_min, e);
        vis_prev = false;
      }
      d += std::max(30.0, d * 0.004);
    }
    // Still hidden at the end of the ray: cand is its highest point = what the sky meets. (Visible at the end: the
    // terrain still climbs beyond the range or the data ran out, so there is no skyline in this column.) Unlike a
    // ridge in front, the skyline needs no dip behind it: a far ridge just peeking over a nearer one is still the
    // skyline, and leaving it out broke the line into pieces (Sanya 08, 11/21).
    if (cand && !cut) {
      cand->sky = true;
      cand->drop = cand->e - hid_min;
      (cand->h > 0.5 && cand->e >= sky_above ? sky : low)[i] = *cand;
    }
  }
  // join ridges of neighbouring columns at about the same range and angle into lines
  struct Open {
    size_t line;
    Sil s;
  };
  std::vector<std::vector<Sil>> chains;
  std::vector<Open> open;
  const double max_de = std::tan(rad(0.6));
  for (int i = 0; i < n; ++i) {
    std::vector<Open> next;
    std::vector<bool> used(open.size(), false);
    for (const Sil& s : cols[i]) {
      int best = -1;
      double bd = 1e9;
      for (size_t k = 0; k < open.size(); ++k) {
        if (used[k]) continue;
        const double rel = std::abs(open[k].s.d - s.d) / s.d;
        if (rel < 0.12 && std::abs(open[k].s.e - s.e) < max_de && rel < bd) bd = rel, best = static_cast<int>(k);
      }
      size_t line;
      if (best >= 0) {
        used[best] = true;
        line = open[best].line;
      } else {
        line = chains.size();
        chains.emplace_back();
      }
      chains[line].push_back(s);
      next.push_back({line, s});
    }
    open = std::move(next);
  }
  const double inner_drop = std::tan(rad(rp.inner_drop_deg));
  for (const auto& c : chains) {
    double drop = 0;
    for (const auto& s : c) drop += s.drop;
    drop /= c.size();
    if (rp.sky_only || c.size() < static_cast<size_t>(rp.inner_min_cols) || drop < inner_drop)
      continue;  // lone specks and faint folds in front of the skyline: left out
    std::vector<TerrainPt> l;
    l.reserve(c.size());
    for (const auto& s : c) l.push_back({s.la, s.lo, s.h, s.h >= warn_h});
    out.lines.push_back(std::move(l));
  }
  // The skyline: one line through every column that has one, whatever the range (where the skyline passes from a
  // near ridge to a far one, the two neighbouring points are joined: on the screen they are 0.2 deg apart). Broken
  // only where a column has none (sea, low plain, missing tile); a gap of up to sky_bridge_deg between two pieces is
  // closed with the low points in it (a saddle down to the horizon reads as one line); specks shorter than
  // sky_min_cols columns are left out.
  const int bridge = static_cast<int>(std::floor(rp.sky_bridge_deg / rp.az_step + 1e-9));
  for (int i = 0; i < n;) {
    if (sky[i]) {
      ++i;
      continue;
    }
    int j = i;
    while (j < n && !sky[j] && low[j]) ++j;
    if (i > 0 && sky[i - 1] && j < n && sky[j] && j - i <= bridge)
      for (int k = i; k < j; ++k) sky[k] = low[k];
    i = std::max(j, i + 1);
  }
  std::vector<TerrainPt> l;
  auto flush = [&] {
    if (l.size() >= static_cast<size_t>(std::max(2, rp.sky_min_cols))) out.lines.push_back(std::move(l));
    l.clear();
  };
  for (int i = 0; i < n; ++i) {
    if (!sky[i]) {
      flush();
      continue;
    }
    const Sil& s = *sky[i];
    l.push_back({s.la, s.lo, s.h, s.h >= warn_h});
  }
  flush();
  return out;
}

TerrainMesh build_fill(Dem& dem, double lat, double lon, double az_deg, const RidgeParams& rp) {
  TerrainMesh m;
  m.lat0 = lat, m.lon0 = lon;
  const Vec2 p1 = offset_latlon(lat, lon, 0.0, 1000.0), p2 = offset_latlon(lat, lon, 1000.0, 0.0);
  const double dlat_m = (p1.x - lat) / 1000.0, dlon_m = (p2.y - lon) / 1000.0;
  const double R2 = 2.0 * kEarthRadius;
  // ranges: every 25 m close in, then 1.2 % apart (about the same size on the screen as the columns)
  std::vector<double> ds;
  const double d_max = rp.fill_km * 1000.0;
  for (double d = 60.0; d <= d_max; d += std::max(25.0, d * 0.012)) ds.push_back(d);
  const int na = static_cast<int>(std::floor(2 * rp.fill_half / rp.fill_step)) + 1, nd = static_cast<int>(ds.size());
  if (nd < 2) return m;
  std::vector<double> se(na), cn(na);
  for (int i = 0; i < na; ++i) {
    const double az = rad(az_deg - rp.fill_half + i * rp.fill_step);
    se[i] = std::sin(az), cn[i] = std::cos(az);
  }
  // heights; a tile that is not there (yet): 0 and no fill around it (its edge would light up as a cliff)
  std::vector<double> h(static_cast<size_t>(na) * nd);
  std::vector<char> gap(h.size(), 0);
  for (int i = 0; i < na; ++i)
    for (int k = 0; k < nd; ++k) {
      const double d = ds[k];
      const auto z = dem.height(lat + cn[i] * d * dlat_m, lon + se[i] * d * dlon_m);
      h[static_cast<size_t>(i) * nd + k] = z.value_or(0.0);
      gap[static_cast<size_t>(i) * nd + k] = !z;
    }
  auto near_gap = [&](int i, int k) {
    for (int a = std::max(0, i - 1); a <= std::min(na - 1, i + 1); ++a)
      for (int b = std::max(0, k - 1); b <= std::min(nd - 1, k + 1); ++b)
        if (gap[static_cast<size_t>(a) * nd + b]) return true;
    return false;
  };
  // light from the pilot's upper left, fixed to the heading the mesh was built for
  const double sa = rad(az_deg - 70.0);
  Vec3 sun{0.6 * std::sin(sa), 0.6 * std::cos(sa), 0.8};
  sun = (1.0 / std::sqrt(dot(sun, sun))) * sun;
  const double tlo = std::tan(rad(rp.slope_lo)), thi = std::tan(rad(rp.slope_hi));
  m.na = na, m.nd = nd;
  m.range_m = ds.back();
  m.v.resize(static_cast<size_t>(na) * nd * 4);
  auto P = [&](int i, int k) {
    const double d = ds[k];
    return Vec3{se[i] * d, cn[i] * d, h[static_cast<size_t>(i) * nd + k]};
  };
  for (int i = 0; i < na; ++i)
    for (int k = 0; k < nd; ++k) {
      const Vec3 p = P(i, k);
      // normal from the neighbours in the grid: smooths over about one cell (a few metres close in, ~1 km far out)
      const Vec3 ta = P(std::min(i + 1, na - 1), k) - P(std::max(i - 1, 0), k);
      const Vec3 td = P(i, std::min(k + 1, nd - 1)) - P(i, std::max(k - 1, 0));
      Vec3 n = cross(td, ta);
      if (n.z < 0) n = -1.0 * n;
      const double len = std::sqrt(dot(n, n));
      double a = 0.0;
      if (len > 0 && n.z > 0 && !near_gap(i, k)) {
        n = (1.0 / len) * n;
        const double grad = std::sqrt(std::max(0.0, 1.0 - n.z * n.z)) / n.z;  // tan(slope)
        const double st = std::clamp((grad - tlo) / (thi - tlo), 0.0, 1.0);
        a = st * (0.15 + 0.50 * std::max(0.0, dot(n, sun)));
      }
      float* o = &m.v[(static_cast<size_t>(i) * nd + k) * 4];
      o[0] = static_cast<float>(p.x);
      o[1] = static_cast<float>(p.y);
      o[2] = static_cast<float>(p.z - ds[k] * ds[k] / R2);
      o[3] = static_cast<float>(a);
    }
  return m;
}

void TerrainWorker::start(std::filesystem::path dir) {
  stop();
  dir_ = std::move(dir);
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = false;
    have_req_ = false;
    out_.reset();
    nearby_.clear();
    status_.clear();
  }
  th_ = std::thread([this] { run(); });
}

void TerrainWorker::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
  std::lock_guard<std::mutex> lk(mu_);
  out_.reset();
  nearby_.clear();
}

void TerrainWorker::update(double lat, double lon, double alt_m, double hdg_true, double ground_m,
                           const RidgeParams& rp) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    req_ = {lat, lon, alt_m, hdg_true, ground_m, rp};
    have_req_ = true;
  }
  cv_.notify_one();
}

std::shared_ptr<const TerrainLines> TerrainWorker::latest() const {
  std::lock_guard<std::mutex> lk(mu_);
  return out_;
}

std::vector<Dem::Near> TerrainWorker::nearby() const {
  std::lock_guard<std::mutex> lk(mu_);
  return nearby_;
}

std::string TerrainWorker::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  return status_;
}

void TerrainWorker::run() {
  Dem dem(dir_);
  std::optional<Req> done;  // the request the current lines were made for
  double t_done = 0, t_near = -1e9;
  size_t n_missing = 0;
  while (true) {
    Req r;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return stop_ || have_req_; });
      if (stop_) return;
      if (!have_req_) continue;
      r = req_;
    }
    const double t = now_s();
    if (t - t_near > 2.0) {
      // every land tile in range, nearest first: streaming fetches the missing ones and keeps these in the cache
      const double km = std::max(r.rp.d_max, r.rp.fill ? r.rp.fill_km * 1000.0 : 0.0) / 1000.0;
      auto nearby = dem.around(r.lat, r.lon, km + 10.0);
      n_missing = 0;
      for (const auto& n : nearby) n_missing += !n.have && n.d <= km;
      dem.drop_far(r.lat, r.lon, km + 250.0);  // unmap what is far behind (the cache may delete it then)
      t_near = t;
      std::lock_guard<std::mutex> lk(mu_);
      nearby_ = std::move(nearby);
    }
    bool need = !done;
    if (done) {
      const Vec2 m = enu(done->lat, done->lon, r.lat, r.lon);
      double dh = std::fmod(std::abs(r.hdg - done->hdg), 360.0);
      if (dh > 180) dh = 360 - dh;
      const RidgeParams &a = r.rp, &b = done->rp;
      const bool params = a.lines != b.lines || a.sky_only != b.sky_only || a.fill != b.fill || a.fill_km != b.fill_km ||
                          a.d_max != b.d_max || a.d_min != b.d_min || a.min_drop_deg != b.min_drop_deg ||
                          a.inner_drop_deg != b.inner_drop_deg || a.warn_below_m != b.warn_below_m;
      need = params || std::hypot(m.x, m.y) > 250.0 || std::abs(r.alt - done->alt) > 60.0 || dh > 8.0 ||
             t - t_done > 3.0;  // the last also picks up tiles that arrived meanwhile
    }
    if (!need) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    const double t0 = now_s();
    auto lines = std::make_shared<TerrainLines>(
        r.rp.lines ? extract_ridges(dem, r.lat, r.lon, r.alt, r.hdg, r.rp) : TerrainLines{});
    const double ms_lines = (now_s() - t0) * 1000.0;
    if (r.rp.fill) lines->mesh = std::make_shared<TerrainMesh>(build_fill(dem, r.lat, r.lon, r.hdg, r.rp));
    const double ms = (now_s() - t0) * 1000.0;
    const auto here = dem.height(r.lat, r.lon);
    char b[240];
    std::snprintf(b, sizeof b, "%zu lines, %.0f ms (lines %.0f), %d tiles, %zu not there yet, ground dem %s sim %.0f m",
                  lines->lines.size(), ms, ms_lines, dem.tiles_loaded(), n_missing,
                  here ? std::to_string(static_cast<int>(std::lround(*here))).c_str() : "-", r.ground);
    {
      std::lock_guard<std::mutex> lk(mu_);
      out_ = std::move(lines);
      status_ = b;
    }
    done = r;
    t_done = t;
  }
}

}  // namespace birdie::terrain
