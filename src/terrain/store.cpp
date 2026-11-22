// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "terrain/store.h"

#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <utility>

#include "miniz.h"
#include "terrain/geotiff.h"

#pragma comment(lib, "winhttp.lib")

namespace birdie::terrain {
namespace fs = std::filesystem;

namespace {

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

constexpr double kRetryFailedS = 90.0;  // a tile that failed is asked for again after this
constexpr double kNetWaitS = 20.0;      // no network: pause this long
constexpr uint64_t kDiskReserve = 1ull << 30;  // keep at least 1 GB free on the drive

// Copernicus_DSM_COG_10_N34_00_W119_00_DEM (10 = 1 arc-second = GLO-30, 30 = 3 arc-seconds = GLO-90)
std::string cop_name(const std::string& tile, int res) {
  int la = 0, lo = 0;
  parse_tile_name(tile, &la, &lo);
  char b[64];
  std::snprintf(b, sizeof b, "Copernicus_DSM_COG_%s_%c%02d_00_%c%03d_00_DEM", res == 30 ? "10" : "30", tile[0],
                std::abs(la), tile[3], std::abs(lo));
  return b;
}

// GET https://host/path into out. 200 -> 0, 404 / 403 (the buckets say 403 for a missing key) -> 1, else -> 2.
int http_get(const wchar_t* host, const std::wstring& path, std::vector<uint8_t>& out, const std::atomic<bool>& cancel,
             const std::function<void(uint64_t, uint64_t)>& progress) {
  out.clear();
  int rc = 2;
  HINTERNET s = WinHttpOpen(L"Birdie-terrain/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return 2;
  WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
  HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                  : nullptr;
  if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(r, nullptr)) {
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &len, WINHTTP_NO_HEADER_INDEX);
    if (status == 404 || status == 403) {
      rc = 1;
    } else if (status == 200) {
      DWORD64 total = 0;
      wchar_t cl[32] = {};
      DWORD cll = sizeof cl;
      if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cll,
                              WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(cl, nullptr, 10);
      if (total > 0 && total < (1ull << 31)) out.reserve(static_cast<size_t>(total));
      std::vector<uint8_t> buf(1 << 20);
      bool ok = true;
      while (true) {
        if (cancel) {
          ok = false;
          break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(r, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
          ok = false;
          break;
        }
        if (got == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + got);
        if (progress) progress(out.size(), total);
      }
      rc = ok && (total == 0 || out.size() == total) ? 0 : 2;
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return rc;
}

bool read_file(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const auto n = static_cast<size_t>(f.tellg());
  out.resize(n);
  f.seekg(0);
  return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n)));
}

uint64_t free_bytes(const fs::path& p) {
  ULARGE_INTEGER avail{};
  fs::path q = p;
  std::error_code ec;
  while (!q.empty() && !fs::exists(q, ec)) q = q.parent_path();
  if (q.empty() || !GetDiskFreeSpaceExW(q.c_str(), &avail, nullptr, nullptr)) return 0;
  return avail.QuadPart;
}

bool ends_with(std::string s, const char* tail) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  const size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// a DEM GeoTIFF inside a Copernicus archive (they also hold quality layers: EDM, FLM, HEM, WBM, also .tif)
bool is_dem_tif(const std::string& name) { return ends_with(name, "_dem.tif") || ends_with(name, "_dem.tiff"); }

}  // namespace

const std::vector<Region>& regions() {
  static const std::vector<Region> r = {
      {"cn_sw", "中国西南（云贵川渝）", "SW China (Yunnan, Guizhou, Sichuan)", 21, 34, 97, 110},
      {"tibet", "青藏高原", "Tibetan Plateau", 27, 37, 78, 104},
      {"cn_se", "中国东南沿海", "SE China coast", 18, 31, 105, 123},
      {"cn_n", "华北 · 华中", "North & central China", 30, 42, 108, 123},
      {"cn_ne", "东北", "NE China", 38, 54, 118, 135},
      {"cn_nw", "新疆 · 甘肃", "Xinjiang & Gansu", 35, 49, 73, 105},
      {"taiwan", "台湾", "Taiwan", 21, 26, 119, 123},
      {"korea", "韩国", "Korea", 33, 39, 124, 131},
      {"japan", "日本", "Japan", 30, 46, 128, 146},
      {"himalaya", "尼泊尔 · 喜马拉雅", "Nepal & Himalaya", 26, 31, 80, 89},
      {"alps", "阿尔卑斯", "Alps", 43, 48, 5, 17},
      {"iberia", "伊比利亚南部 · 直布罗陀", "Southern Iberia & Gibraltar", 35, 39, -8, -1},
      {"norway", "挪威", "Norway", 58, 71, 4, 31},
      {"us_west", "美国西部山区", "Western US mountains", 31, 49, -125, -104},
      {"alaska", "阿拉斯加南部", "Southern Alaska", 55, 63, -160, -130},
      {"mexico", "墨西哥中南部", "Central & southern Mexico", 15, 22, -106, -94},
      {"nz", "新西兰", "New Zealand", -47, -34, 166, 179},
  };
  return r;
}

std::vector<std::string> region_tiles(const Region& r) {
  std::vector<std::string> v;
  for (int la = r.south; la < r.north; ++la)
    for (int lo = r.west; lo < r.east; ++lo)
      if (is_land(la, lo)) v.push_back(tile_name(la, lo));
  return v;
}

std::vector<std::string> tiles_around(double lat, double lon, double km) {
  const double kPi = 3.14159265358979323846;
  const double c = std::max(0.1, std::cos(lat * kPi / 180));
  std::vector<std::pair<double, std::string>> v;
  for (int la = static_cast<int>(std::floor(lat - km / 111)); la <= static_cast<int>(std::floor(lat + km / 111)); ++la)
    for (int lo = static_cast<int>(std::floor(lon - km / (111 * c))); lo <= static_cast<int>(std::floor(lon + km / (111 * c)));
         ++lo) {
      const int l = ((lo + 180) % 360 + 360) % 360 - 180;
      if (la < -90 || la > 89 || !is_land(la, l)) continue;
      const double clat = std::clamp(lat, double(la), la + 1.0), clon = std::clamp(lon, double(lo), lo + 1.0);
      const double d = std::hypot((clat - lat) * 111, (clon - lon) * 111 * c);
      if (d <= km) v.push_back({d, tile_name(la, l)});
    }
  std::sort(v.begin(), v.end());
  std::vector<std::string> out;
  for (auto& [d, n] : v) out.push_back(n);
  return out;
}

uint64_t tile_bytes(int ilat, int res) {
  const int a = ilat >= 0 ? ilat : -ilat - 1;  // the cell's latitude band (its edge nearer the pole)
  const int full = res == 30 ? 3600 : 1200;
  const int w = a < 50 ? full : a < 60 ? full * 2 / 3 : a < 70 ? full / 2 : a < 80 ? full / 3 : a < 85 ? full / 5 : full / 10;
  return 56 + 2ull * full * w;
}

fs::path default_dir() {
  PWSTR la = nullptr;
  fs::path p;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la))) p = fs::path(la) / L"Birdie" / L"dem";
  CoTaskMemFree(la);
  return p;
}

// ---------------------------------------------------------------- Store
Store::Store() : th_([this] { run(); }) {}

Store::~Store() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
  }
  cancel_ = true;
  cv_.notify_all();
  th_.join();
}

void Store::set_dir(const fs::path& dir) {
  std::lock_guard<std::mutex> lk(mu_);
  if (dir == dir_) return;
  dir_ = dir;
  want_.clear();
  keep_.clear();
  pkg_.clear();
  failed_at_.clear();
  cancel_ = true;
  t_stats_ = -1e9;
  cv_.notify_all();
}

fs::path Store::dir() const {
  std::lock_guard<std::mutex> lk(mu_);
  return dir_;
}

bool Store::have(const std::string& tile, int res, bool cache_ok) const {
  std::error_code ec;
  const fs::path pkg = dir_ / (tile + ".bdem");
  if (fs::exists(dir_ / (tile + ".none"), ec)) return true;  // known: no tile there (sea)
  if (fs::exists(pkg, ec)) return cache_ok || res == 90 || bdem_res(pkg) == 30;
  return cache_ok && fs::exists(dir_ / L"cache" / (tile + ".bdem"), ec);
}

void Store::stream(const std::vector<Dem::Near>& nearby, double reach_km, int res, uint64_t cap_bytes) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (dir_.empty()) return;
    stream_on_ = true;
    cap_ = cap_bytes;
    keep_.clear();
    want_.clear();
    const double t = now_s();
    for (const auto& n : nearby) {
      keep_.insert(n.name);
      if (n.have || n.d > reach_km) continue;
      if (const auto it = failed_at_.find(n.name); it != failed_at_.end() && t - it->second < kRetryFailedS) continue;
      if (n.name == prog_.tile && prog_.kind == "stream") continue;  // on its way
      if (!have(n.name, res, true)) want_.push_back({n.name, res, true});
    }
  }
  if (now_s() - t_touch_ > 30) {  // the tiles in use count as just used (the oldest go first when trimming)
    t_touch_ = now_s();
    touch(nearby);
  }
  cv_.notify_all();
}

void Store::touch(const std::vector<Dem::Near>& keep) {
  const fs::path cache = dir() / L"cache";
  std::error_code ec;
  const auto now = fs::file_time_type::clock::now();
  for (const auto& n : keep) {
    const fs::path p = cache / (n.name + ".bdem");
    if (fs::exists(p, ec)) fs::last_write_time(p, now, ec);
  }
}

void Store::stop_stream() {
  std::lock_guard<std::mutex> lk(mu_);
  if (!stream_on_) return;
  stream_on_ = false;
  want_.clear();
  if (prog_.kind == "stream") cancel_ = true;
}

bool Store::streaming() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stream_on_;
}

void Store::install(const std::string& label, const std::vector<std::string>& tiles, int res) {
  std::lock_guard<std::mutex> lk(mu_);
  if (dir_.empty()) return;
  if (pkg_.empty()) pkg_done_ = pkg_total_ = pkg_failed_ = 0;
  pkg_label_ = label;
  for (const auto& t : tiles) {
    if (have(t, res, false)) continue;
    if (std::any_of(pkg_.begin(), pkg_.end(), [&](const Job& j) { return j.tile == t; })) continue;
    pkg_.push_back({t, res, false});
    ++pkg_total_;
  }
  prog_.error.clear();
  cv_.notify_all();
}

void Store::stop_packages() {
  std::lock_guard<std::mutex> lk(mu_);
  pkg_.clear();
  if (prog_.kind == "package") cancel_ = true;
}

int Store::delete_packages(const std::vector<std::string>& tiles) {
  std::lock_guard<std::mutex> lk(mu_);
  std::error_code ec;
  int n = 0;
  if (tiles.empty()) {
    for (const auto& e : fs::directory_iterator(dir_, ec))
      if (e.is_regular_file(ec) && (e.path().extension() == L".bdem" || e.path().extension() == L".none"))
        n += fs::remove(e.path(), ec) ? 1 : 0;
  } else {
    for (const auto& t : tiles) n += fs::remove(dir_ / (t + ".bdem"), ec) ? 1 : 0;
  }
  t_stats_ = -1e9;
  return n;
}

int Store::count_packages(const std::vector<std::string>& tiles) const {
  std::lock_guard<std::mutex> lk(mu_);
  std::error_code ec;
  int n = 0;
  for (const auto& t : tiles) n += fs::exists(dir_ / (t + ".bdem"), ec) ? 1 : 0;
  return n;
}

int Store::clear_cache() {
  std::lock_guard<std::mutex> lk(mu_);
  std::error_code ec;
  int n = 0;
  for (const auto& e : fs::directory_iterator(dir_ / L"cache", ec))
    if (e.is_regular_file(ec)) n += fs::remove(e.path(), ec) ? 1 : 0;  // mapped tiles go once they are let go
  t_stats_ = -1e9;
  return n;
}

void Store::trim_cache(uint64_t cap_bytes) {
  std::lock_guard<std::mutex> lk(mu_);
  trim_locked(cap_bytes, keep_);
}

void Store::trim_locked(uint64_t cap_bytes, const std::set<std::string>& keep) {
  struct F {
    fs::path p;
    uint64_t size;
    fs::file_time_type t;
  };
  std::vector<F> files;
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir_ / L"cache", ec)) {
    if (!e.is_regular_file(ec) || e.path().extension() != L".bdem") continue;
    const uint64_t sz = e.file_size(ec);
    total += sz;
    if (!keep.count(e.path().stem().string())) files.push_back({e.path(), sz, e.last_write_time(ec)});
  }
  if (total <= cap_bytes) return;
  std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
  for (const auto& f : files) {
    if (total <= cap_bytes) break;
    if (fs::remove(f.p, ec)) total -= f.size;
  }
  t_stats_ = -1e9;
}

void Store::import_folder() {
  std::lock_guard<std::mutex> lk(mu_);
  want_import_ = true;
  cv_.notify_all();
}

bool Store::move_to(const fs::path& to_in, std::string* err) {
  std::error_code ec;
  const fs::path to = fs::weakly_canonical(to_in, ec);
  const fs::path from = fs::weakly_canonical(dir(), ec);
  auto inside = [](const fs::path& a, const fs::path& b) {  // a is b or below it
    auto ia = a.begin(), ib = b.begin();
    for (; ib != b.end(); ++ia, ++ib)
      if (ia == a.end() || _wcsicmp(ia->c_str(), ib->c_str()) != 0) return false;
    return true;
  };
  if (to.empty() || inside(to, from) || inside(from, to)) {
    if (err) *err = "same_place";
    return false;
  }
  fs::create_directories(to, ec);
  const fs::path probe = to / L".birdie-write-test";
  { std::ofstream(probe) << "x"; }
  if (!fs::exists(probe, ec)) {
    if (err) *err = "not_writable";
    return false;
  }
  fs::remove(probe, ec);
  std::lock_guard<std::mutex> lk(mu_);
  want_.clear();
  pkg_.clear();
  cancel_ = true;
  want_move_ = to;
  cv_.notify_all();
  return true;
}

StoreStats Store::stats() {
  std::lock_guard<std::mutex> lk(mu_);
  const double t = now_s();
  if (t - t_stats_ < 2.0) return stats_;
  t_stats_ = t;
  StoreStats s;
  const std::u8string u8 = dir_.u8string();
  s.dir.assign(u8.begin(), u8.end());
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir_, ec))
    if (e.is_regular_file(ec) && e.path().extension() == L".bdem") ++s.pkg_tiles, s.pkg_bytes += e.file_size(ec);
  for (const auto& e : fs::directory_iterator(dir_ / L"cache", ec))
    if (e.is_regular_file(ec) && e.path().extension() == L".bdem") ++s.cache_tiles, s.cache_bytes += e.file_size(ec);
  for (const auto& e : fs::directory_iterator(dir_ / L"import", ec)) s.import_files += e.is_regular_file(ec) ? 1 : 0;
  s.disk_free = free_bytes(dir_);
  stats_ = s;
  return s;
}

StoreProgress Store::progress() const {
  std::lock_guard<std::mutex> lk(mu_);
  StoreProgress p = prog_;
  if (p.kind == "stream") {  // this burst: fetched so far, and how many in all with the one on its way
    p.done = stream_got_;
    p.total = stream_got_ + static_cast<int>(want_.size()) + 1;
  } else if (p.kind == "package" || (p.kind.empty() && !pkg_.empty())) {
    p.label = pkg_label_, p.done = pkg_done_, p.total = pkg_total_, p.failed = pkg_failed_;
  }
  return p;
}

std::vector<StoreEvent> Store::take_events() {
  std::lock_guard<std::mutex> lk(mu_);
  return std::exchange(events_, {});
}

bool Store::take_changed() {
  std::lock_guard<std::mutex> lk(mu_);
  return std::exchange(changed_, false);
}

void Store::push_event(StoreEvent e) { events_.push_back(std::move(e)); }  // under mu_

void Store::set_error(const std::string& e) {
  std::lock_guard<std::mutex> lk(mu_);
  prog_.error = e;
}

// One tile: download (30 m, or 90 m where there is no 30 m one) and convert into the cache or the packages.
bool Store::fetch(const Job& j, std::string* err) {
  fs::path dir;
  {
    std::lock_guard<std::mutex> lk(mu_);
    dir = dir_;
  }
  int la = 0, lo = 0;
  parse_tile_name(j.tile, &la, &lo);
  const uint64_t need = tile_bytes(la, j.res) * 2 + kDiskReserve;
  if (free_bytes(dir) < need) {
    *err = "disk_full";
    return false;
  }
  std::vector<uint8_t> tif;
  int rc = 1;
  for (int res : {j.res, j.res == 30 ? 90 : 30}) {
    const std::string cn = cop_name(j.tile, res);
    const std::wstring host = res == 30 ? L"copernicus-dem-30m.s3.amazonaws.com" : L"copernicus-dem-90m.s3.amazonaws.com";
    const std::wstring path = L"/" + std::wstring(cn.begin(), cn.end()) + L"/" + std::wstring(cn.begin(), cn.end()) + L".tif";
    double t_last = now_s();
    uint64_t got_last = 0;
    rc = http_get(host.c_str(), path, tif, cancel_, [&](uint64_t got, uint64_t total) {
      const double t = now_s();
      std::lock_guard<std::mutex> lk(mu_);
      prog_.got = got, prog_.size = total;
      if (t - t_last > 0.5) {
        const double r = (got - got_last) / (t - t_last);
        prog_.rate = prog_.rate > 0 ? 0.6 * prog_.rate + 0.4 * r : r;
        t_last = t, got_last = got;
      }
    });
    if (rc != 1) break;  // found (or a real failure): do not try the other resolution
  }
  if (cancel_) {
    *err = "stopped";
    return false;
  }
  if (rc == 1) {  // neither bucket has it: treat as sea from now on
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream(dir / (j.tile + ".none"));
    return true;
  }
  if (rc != 0) {
    *err = "network";
    return false;
  }
  BdemInfo info;
  std::string e;
  const fs::path out = j.cache ? dir / L"cache" : dir;
  if (!geotiff_to_bdem(tif, out, &info, &e)) {
    *err = "convert: " + e;
    return false;
  }
  if (!j.cache) {  // installed as a package: a copy in the cache is no longer needed
    std::error_code ec;
    fs::remove(dir / L"cache" / (info.name + ".bdem"), ec);
  }
  return true;
}

void Store::run() {
  while (true) {
    Job j;
    bool is_stream = false;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(500), [&] {
        const bool net = now_s() >= net_wait_until_;
        return quit_ || want_import_ || want_move_ || (stream_on_ && !want_.empty() && net) || (!pkg_.empty() && net);
      });
      if (quit_) return;
      cancel_ = false;
      if (want_move_) {
        const fs::path to = *want_move_;
        want_move_.reset();
        lk.unlock();
        do_move(to);
        continue;
      }
      if (want_import_) {
        want_import_ = false;
        lk.unlock();
        do_import();
        continue;
      }
      const bool net = now_s() >= net_wait_until_;
      if (stream_on_ && !want_.empty() && net) {
        j = want_.front();
        want_.erase(want_.begin());
        is_stream = true;
        if (!stream_busy_) {  // the first tile after a quiet spell: say how much is coming
          stream_busy_ = true;
          stream_got_ = 0;
          stream_bytes_ = 0;
          uint64_t bytes = 0;
          int la = 0, lo = 0;
          for (const auto& w : want_)
            if (parse_tile_name(w.tile, &la, &lo)) bytes += tile_bytes(la, w.res);
          if (parse_tile_name(j.tile, &la, &lo)) bytes += tile_bytes(la, j.res);
          push_event({StoreEvent::kStreamStart, static_cast<int>(want_.size()) + 1, bytes, ""});
        }
      } else if (!pkg_.empty() && net) {
        j = pkg_.front();
        pkg_.pop_front();
      } else {
        if (stream_busy_ && want_.empty()) {
          stream_busy_ = false;
          if (stream_got_ > 0) push_event({StoreEvent::kStreamDone, stream_got_, stream_bytes_, ""});
        }
        if (prog_.kind == "stream" || prog_.kind == "package") prog_ = StoreProgress{"", "", 0, 0, 0, "", 0, 0, 0, prog_.error};
        continue;
      }
      prog_.kind = is_stream ? "stream" : "package";
      prog_.tile = j.tile;
      prog_.got = prog_.size = 0;
      if (have(j.tile, j.res, is_stream)) {  // got there meanwhile
        if (!is_stream) ++pkg_done_;
        continue;
      }
    }
    std::string err;
    const bool ok = fetch(j, &err);
    std::lock_guard<std::mutex> lk(mu_);
    changed_ = changed_ || ok;
    t_stats_ = -1e9;
    if (is_stream) {
      if (ok) {
        ++stream_got_;
        int la = 0, lo = 0;
        if (parse_tile_name(j.tile, &la, &lo)) stream_bytes_ += tile_bytes(la, j.res);
        trim_locked(cap_, keep_);
      } else if (err != "stopped") {
        failed_at_[j.tile] = now_s();
        if (err == "network") net_wait_until_ = now_s() + kNetWaitS;
        if (prog_.error != err) push_event({StoreEvent::kStreamFailed, 0, 0, err});
      }
    } else {
      if (ok) {
        ++pkg_done_;
      } else if (err == "network") {  // try again in a while
        pkg_.push_front(j);
        net_wait_until_ = now_s() + kNetWaitS;
      } else if (err != "stopped") {
        ++pkg_failed_;
        if (err == "disk_full") pkg_.clear();
      }
      if (pkg_.empty() && err != "stopped")
        push_event({StoreEvent::kPackageDone, pkg_done_, 0, pkg_label_});
    }
    if (ok) prog_.error.clear();
    else if (err != "stopped") prog_.error = err;
  }
}

void Store::do_import() {
  fs::path dir;
  {
    std::lock_guard<std::mutex> lk(mu_);
    dir = dir_;
    prog_ = StoreProgress{"import", "", 0, 0, 0, "", 0, 0, 0, ""};
  }
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir / L"import", ec))
    if (e.is_regular_file(ec)) files.push_back(e.path());
  int n = 0, bad = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    prog_.total = static_cast<int>(files.size());
  }
  auto convert = [&](const std::vector<uint8_t>& tif) {
    BdemInfo info;
    std::string e;
    if (geotiff_to_bdem(tif, dir, &info, &e)) {
      fs::remove(dir / L"cache" / (info.name + ".bdem"), ec);
      ++n;
      return true;
    }
    return false;
  };
  for (const auto& f : files) {
    const std::string name = f.filename().string();
    bool any = false;
    if (ends_with(name, ".tif") || ends_with(name, ".tiff")) {
      std::vector<uint8_t> b;
      any = read_file(f, b) && convert(b);
    } else if (ends_with(name, ".zip")) {
      mz_zip_archive z{};
      if (mz_zip_reader_init_file(&z, f.string().c_str(), 0)) {
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
          char nm[512];
          mz_zip_reader_get_filename(&z, i, nm, sizeof nm);
          if (!is_dem_tif(nm)) continue;
          size_t sz = 0;
          void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
          if (!p) continue;
          std::vector<uint8_t> b(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + sz);
          mz_free(p);
          any = convert(b) || any;
        }
        mz_zip_reader_end(&z);
      }
    } else if (ends_with(name, ".tar")) {
      std::ifstream t(f, std::ios::binary);
      char h[512];
      while (t.read(h, 512)) {
        if (h[0] == 0) break;
        std::string nm(h, strnlen(h, 100));
        const std::string prefix(h + 345, strnlen(h + 345, 155));
        if (!prefix.empty()) nm = prefix + "/" + nm;
        const uint64_t sz = std::strtoull(std::string(h + 124, 12).c_str(), nullptr, 8);
        const uint64_t padded = (sz + 511) / 512 * 512;
        if ((h[156] == '0' || h[156] == 0) && is_dem_tif(nm) && sz < (1ull << 31)) {
          std::vector<uint8_t> b(static_cast<size_t>(sz));
          t.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(sz));
          t.seekg(static_cast<std::streamoff>(padded - sz), std::ios::cur);
          any = convert(b) || any;
        } else {
          t.seekg(static_cast<std::streamoff>(padded), std::ios::cur);
        }
      }
    }
    if (any) fs::remove(f, ec);  // converted: the original goes (the tool said so)
    else ++bad;
    std::lock_guard<std::mutex> lk(mu_);
    ++prog_.done;
  }
  std::lock_guard<std::mutex> lk(mu_);
  prog_ = StoreProgress{};
  changed_ = changed_ || n > 0;
  t_stats_ = -1e9;
  push_event({StoreEvent::kImportDone, n, 0, bad ? std::to_string(bad) : ""});
}

void Store::do_move(fs::path to) {
  fs::path from;
  {
    std::lock_guard<std::mutex> lk(mu_);
    from = dir_;
    prog_ = StoreProgress{"move", "", 0, 0, 0, "", 0, 0, 0, ""};
  }
  std::error_code ec;
  std::vector<fs::path> rel;
  for (const auto& sub : {fs::path(), fs::path(L"cache"), fs::path(L"import")})
    for (const auto& e : fs::directory_iterator(from / sub, ec))
      if (e.is_regular_file(ec)) rel.push_back(sub / e.path().filename());
  {
    std::lock_guard<std::mutex> lk(mu_);
    prog_.total = static_cast<int>(rel.size());
  }
  auto move_one = [&](const fs::path& a, const fs::path& b) {
    std::error_code e2;
    fs::create_directories(b.parent_path(), e2);
    fs::rename(a, b, e2);
    if (!e2) return true;
    e2.clear();  // another drive: copy, then delete
    if (!fs::copy_file(a, b, fs::copy_options::overwrite_existing, e2)) return false;
    fs::remove(a, e2);
    return true;
  };
  std::vector<fs::path> moved;
  bool ok = true;
  for (const auto& r : rel) {
    if (!move_one(from / r, to / r)) {
      ok = false;
      break;
    }
    moved.push_back(r);
    std::lock_guard<std::mutex> lk(mu_);
    ++prog_.done;
  }
  if (!ok)  // put back what went over, so the data stays in one place
    for (const auto& r : moved) move_one(to / r, from / r);
  std::lock_guard<std::mutex> lk(mu_);
  prog_ = StoreProgress{};
  if (ok) {
    for (const auto& sub : {L"cache", L"import"}) fs::remove(from / sub, ec);  // only when empty
    fs::remove(from, ec);
    dir_ = to;
    failed_at_.clear();
    push_event({StoreEvent::kMoveDone, static_cast<int>(moved.size()), 0, ""});
  } else {
    push_event({StoreEvent::kMoveFailed, 0, 0, "move"});
  }
  t_stats_ = -1e9;
}

}  // namespace birdie::terrain
