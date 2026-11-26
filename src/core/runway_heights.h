// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Runway heights before the wheels are on the pavement.
//
// The facility ALTITUDE MSFS gives for a runway can be metres off the surface the scenery actually has (ZULS 11/22:
// 10L +11.7 m, 10R +17.3 m), so the outline floats above the runway on the approach. Over the pavement Birdie
// measures the real surface from the ground under the aircraft (anchor_from_ground). Until then, best first:
//   ground    measured now (anchor_from_ground)
//   saved     measured on an earlier flight, remembered in runway_heights.json (while the facility height is unchanged)
//   airport   another runway of the airport was measured: the DEM where it agrees with that measurement there and
//             is flat along it (within kDemFlatM), else the same offset from the facility height
//   dem       the terrain heights (Copernicus, terrain/), along the centreline, when those tiles are on disk
//   facility  as MSFS gives it
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/runway.h"

namespace birdie {

enum AltSrc : int { kAltFacility = 0, kAltDem = 1, kAltAirport = 2, kAltSaved = 3, kAltGround = 4 };
const char* alt_src_name(int src);

// Heights measured on earlier flights: <icao> -> <end name> -> centre height and the facility height it was measured
// against (a different facility height means other scenery: the old measurement no longer counts).
class RunwayHeights {
 public:
  RunwayHeights() = default;
  explicit RunwayHeights(std::filesystem::path file);
  std::optional<double> get(const Runway& r) const;
  // Remembers a measured end; true when that changed what is kept (by more than 0.3 m).
  bool put(const Runway& r);
  bool save() const;  // false = could not write
  size_t size() const;

 private:
  struct Entry {
    double alt, fac;
  };
  std::filesystem::path file_;
  std::map<std::string, std::map<std::string, Entry>> by_icao_;
};

// The terrain height at a point, nullopt = not available (terrain/terrain.h Dem::height).
using DemFn = std::function<std::optional<double>(double lat, double lon)>;

// The DEM's idea of the runway's centre height: five points along the centreline, each taken back to the centre along
// the slope, the median. nullopt when any of them is missing or they spread over more than 30 m (not a runway the
// DEM sees as flat ground).
std::optional<double> dem_centre(const Runway& r, const DemFn& dem);
// How far those five points spread (highest - lowest), nullopt when any is missing.
std::optional<double> dem_spread(const Runway& r, const DemFn& dem);
// A measured runway whose DEM spreads more than this is on flattened scenery: the DEM is not used for the others.
constexpr double kDemFlatM = 3.0;

// Improves the ends of ONE airport that are not measured yet (alt_src < kAltSaved stays open to better sources).
// Returns, for the log, one line per end whose height source changed.
std::vector<std::string> refine_heights(const std::vector<Runway*>& airport, const RunwayHeights& saved,
                                        const DemFn& dem);

}  // namespace birdie
