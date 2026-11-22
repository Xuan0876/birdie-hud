// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "core/runway_heights.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

#include "core/geo.h"

namespace birdie {

const char* alt_src_name(int src) {
  switch (src) {
    case kAltGround: return "ground";
    case kAltSaved: return "saved";
    case kAltAirport: return "airport";
    case kAltDem: return "dem";
    default: return "facility";
  }
}

RunwayHeights::RunwayHeights(std::filesystem::path file) : file_(std::move(file)) {
  std::ifstream f(file_);
  if (!f) return;
  try {
    const auto j = nlohmann::json::parse(f);
    if (!j.is_object()) return;
    for (const auto& [icao, ends] : j.items()) {
      if (!ends.is_object()) continue;
      for (const auto& [name, e] : ends.items())
        if (e.is_object() && e.contains("alt") && e["alt"].is_number() && e.contains("fac") && e["fac"].is_number())
          by_icao_[icao][name] = {e["alt"].get<double>(), e["fac"].get<double>()};
    }
  } catch (const std::exception&) {
    by_icao_.clear();  // a broken file: start again, it is only a cache
  }
}

std::optional<double> RunwayHeights::get(const Runway& r) const {
  const auto a = by_icao_.find(r.icao);
  if (a == by_icao_.end()) return std::nullopt;
  const auto e = a->second.find(r.name);
  if (e == a->second.end() || std::abs(e->second.fac - r.facility_alt_m) > 0.5) return std::nullopt;
  return e->second.alt;
}

bool RunwayHeights::put(const Runway& r) {
  if (!r.anchored || r.icao.empty() || r.name.empty() || !std::isfinite(r.alt_m)) return false;
  auto& e = by_icao_[r.icao][r.name];
  const bool changed = std::abs(e.alt - r.alt_m) > 0.3 || std::abs(e.fac - r.facility_alt_m) > 0.05;
  if (changed) e = {r.alt_m, r.facility_alt_m};
  return changed;
}

bool RunwayHeights::save() const {
  if (file_.empty()) return false;
  nlohmann::json j = nlohmann::json::object();
  auto r1 = [](double x) { return std::round(x * 10) / 10; };
  for (const auto& [icao, ends] : by_icao_)
    for (const auto& [name, e] : ends) j[icao][name] = {{"alt", r1(e.alt)}, {"fac", r1(e.fac)}};
  const auto tmp = file_.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << j.dump(1);
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file_, ec);
  return !ec;
}

size_t RunwayHeights::size() const {
  size_t n = 0;
  for (const auto& [icao, ends] : by_icao_) n += ends.size();
  return n;
}

std::optional<double> dem_centre(const Runway& r, const DemFn& dem) {
  if (!dem) return std::nullopt;
  const double h = rad(r.hdg), rise = std::tan(rad(r.slope));
  std::vector<double> c;
  for (const double f : {-0.4, -0.2, 0.0, 0.2, 0.4}) {
    const double along = f * r.length;
    const Vec2 ll = offset_latlon(r.lat, r.lon, along * std::sin(h), along * std::cos(h));
    const auto z = dem(ll.x, ll.y);
    if (!z) return std::nullopt;
    c.push_back(*z - along * rise);
  }
  std::sort(c.begin(), c.end());
  if (c.back() - c.front() > 30.0) return std::nullopt;
  return c[c.size() / 2];
}

std::vector<std::string> refine_heights(const std::vector<Runway*>& airport, const RunwayHeights& saved,
                                        const DemFn& dem) {
  std::vector<std::string> log;
  auto say = [&](const Runway& r) {
    char b[200];
    std::snprintf(b, sizeof b, "[rwy] %s %s height %.1f m from %s (facility %.1f, %+.1f m)", r.icao.c_str(),
                  r.name.c_str(), r.alt_m, alt_src_name(r.alt_src), r.facility_alt_m, r.alt_m - r.facility_alt_m);
    log.push_back(b);
  };
  // the airport's measured ends (now or on an earlier flight) first
  for (Runway* r : airport)
    if (!r->anchored && r->alt_src < kAltSaved)
      if (const auto s = saved.get(*r)) {
        r->alt_m = *s, r->alt_src = kAltSaved;
        say(*r);
      }
  const Runway* ref = nullptr;
  for (const Runway* r : airport)
    if (r->alt_src >= kAltSaved && (!ref || r->alt_src > ref->alt_src)) ref = r;
  // how well the DEM matches this scenery where it has been measured (third-party airports flatten the ground)
  std::optional<double> dem_err;
  if (ref)
    if (const auto d = dem_centre(*ref, dem)) dem_err = *d - ref->alt_m;
  for (Runway* r : airport) {
    if (r->anchored || r->alt_src >= kAltSaved) continue;
    const auto d = dem_centre(*r, dem);
    double alt = r->facility_alt_m;
    int src = kAltFacility;
    if (ref && d && dem_err && std::abs(*dem_err) <= 3.0) {
      alt = *d - *dem_err, src = kAltAirport;  // the DEM, trimmed by what it was off at the measured runway
    } else if (ref) {
      alt = r->facility_alt_m + (ref->alt_m - ref->facility_alt_m), src = kAltAirport;
    } else if (d && std::abs(*d - r->facility_alt_m) < 150.0) {
      alt = *d, src = kAltDem;
    }
    if (src == r->alt_src && std::abs(alt - r->alt_m) < 0.05) continue;
    if (src < r->alt_src) continue;  // never step back to a worse source
    const bool news = src != r->alt_src || std::abs(alt - r->alt_m) > 1.0;  // the DEM may still be filling in
    r->alt_m = alt;
    r->alt_src = src;
    if (news) say(*r);
  }
  return log;
}

}  // namespace birdie
