// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Terrain for the HUD (synthetic vision): a see-through HUD-green surface on the slopes and the skyline.
//
// Heights: Copernicus DEM tiles (GLO-30 or GLO-90), one 1 x 1 degree .bdem file each (terrain/geotiff.h), in the
// terrain folder (default %LOCALAPPDATA%\Birdie\dem): packages the user downloaded on purpose in the folder itself,
// streamed tiles in its cache\ subfolder (terrain/store.h keeps that one under its size limit). Memory-mapped here.
// The Copernicus tile list (built in) tells sea (no tile exists: height 0) from land that is not there yet.
//
// Skyline and ridges: from the eye, every az_step degrees across +-az_half around the heading, a ray is marched
// outward over the DEM (earth curvature included). The highest point of a ray is the skyline; where visible terrain
// gives way to terrain hidden behind it, that last visible point is a ridge. The skyline is one line across the
// columns; ridges in neighbouring columns at about the same range are joined into lines. The points are kept as
// lat / lon / altitude, so they are projected every frame like the runway and stay put while the aircraft moves.
// Fill: a polar mesh around the aircraft (build_fill), drawn by the GPU (terrain/terrain_gpu.h). Both are recomputed
// a few times a second on a background thread (TerrainWorker).
#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace birdie::terrain {

class Dem;

struct TerrainPt {
  double lat, lon, alt;  // metres above MSL
  bool warn;             // at or above the warning height (close to or above the aircraft)
};
// The terrain as a surface for the green fill (HUD green, see-through): a polar grid around the eye where it was
// built, na columns (azimuth, west to east) x nd rows (range, near to far). Per vertex 4 floats: east, north, up
// (metres from the anchor; up = height above MSL minus the earth's curvature from the anchor) and the base opacity
// (slopes light up, flat ground of any height stays clear; the renderer fades it with range). The GPU draws it with
// a depth buffer, so what hides what comes for free.
struct TerrainMesh {
  double lat0 = 0, lon0 = 0;  // anchor
  double range_m = 0;         // the farthest row
  int na = 0, nd = 0;
  std::vector<float> v;
};

struct TerrainLines {
  std::vector<std::vector<TerrainPt>> lines;
  bool skyline_complete = true;  // false: some rays ran into tiles that are not there yet
  std::shared_ptr<const TerrainMesh> mesh;  // the green fill, null = off
};

struct RidgeParams {
  double az_half = 40.0, az_step = 0.2;  // degrees around the heading
  double d_min = 1500.0, d_max = 100000.0;  // metres
  double min_drop_deg = 0.25;  // a ridge counts when what lies behind it dips at least this much below it
  double inner_drop_deg = 0.5;  // ... and one in front of the skyline needs this much (keeps the picture clean)
  double sky_above_horizon_deg = 0.15;  // the skyline: only where it stands this far above the sea horizon
  double sky_bridge_deg = 1.5;          // ... but gaps up to this wide between two pieces are closed
  int sky_min_cols = 4;                 // ... and pieces shorter than this many columns are left out
  int inner_min_cols = 8;       // ... over at least this many columns
  double warn_below_m = 150.0;  // terrain higher than (aircraft - this) is drawn as a warning
  bool lines = true, sky_only = true;  // lines at all; only the skyline (else also the ridges in front of it)
  // green fill
  bool fill = true;
  double fill_km = 60.0;
  double fill_half = 65.0, fill_step = 0.25;  // degrees around the heading
  double slope_lo = 4.0, slope_hi = 24.0;     // slope (deg) where the fill starts / is full: flat ground stays clear
};

// The fill mesh seen from (lat, lon, eye_alt_m), centred on az_deg (true). Light from the pilot's upper left.
TerrainMesh build_fill(Dem& dem, double lat, double lon, double az_deg, const RidgeParams& rp);

class Dem {
 public:
  explicit Dem(std::filesystem::path dir);
  ~Dem();
  Dem(const Dem&) = delete;
  Dem& operator=(const Dem&) = delete;
  // Metres above MSL, bilinear; nullopt = that tile is not available (yet). Sea (no tile) = 0.
  std::optional<double> height(double lat, double lon);
  // Land tiles within km of (lat, lon), nearest first, and whether each is there (package or cache).
  struct Near {
    std::string name;
    double d;  // km to the tile's nearest point
    bool have;
  };
  std::vector<Near> around(double lat, double lon, double km);
  // Unmaps tiles farther than keep_km.
  void drop_far(double lat, double lon, double keep_km);
  int tiles_loaded() const { return static_cast<int>(tiles_.size()); }
  const std::filesystem::path& dir() const { return dir_; }

 private:
  struct Tile {
    void* file = nullptr;
    void* map = nullptr;
    const void* base = nullptr;  // the mapped view
    const short* z = nullptr;  // nullptr + sea = no tile
    bool sea = false;
    int w = 0, h = 0;
    double lat0 = 0, lon0 = 0, dlat = 1, dlon = 1;
  };
  const Tile* tile(int ilat, int ilon);
  std::filesystem::path dir_;
  std::map<long long, Tile> tiles_;          // loaded or known sea
  std::map<long long, double> missing_;      // key -> last time looked for (s)
};

// Whether the Copernicus DEM has a tile for the cell with south-west corner (ilat, ilon), i.e. it holds land.
bool is_land(int ilat, int ilon);
// N34W119 -> (34, -119); false if it is not a tile name.
bool parse_tile_name(const std::string& name, int* ilat, int* ilon);

// Tile name of the 1 x 1 degree cell with south-west corner (ilat, ilon): N34W119.
std::string tile_name(int ilat, int ilon);

// One set of ridge lines seen from (lat, lon, eye_alt_m), centred on az_deg (true).
TerrainLines extract_ridges(Dem& dem, double lat, double lon, double eye_alt_m, double az_deg, const RidgeParams& rp);

// Background worker: keeps one TerrainLines for the latest position.
class TerrainWorker {
 public:
  ~TerrainWorker() { stop(); }
  void start(std::filesystem::path dir);
  void stop();
  bool running() const { return th_.joinable(); }
  // Latest aircraft position (cheap, call every frame). ground_m = the sim's ground height under the aircraft
  // (PLANE ALTITUDE - PLANE ALT ABOVE GROUND), for the DEM check in the log.
  void update(double lat, double lon, double alt_m, double hdg_true, double ground_m, const RidgeParams& rp);
  std::shared_ptr<const TerrainLines> latest() const;
  std::string status() const;  // for the log line
  // The land tiles within range of the latest position, nearest first, and whether each is there: what streaming
  // fetches (the missing ones) and keeps (all of them) in the cache.
  std::vector<Dem::Near> nearby() const;

 private:
  void run();
  std::filesystem::path dir_;
  std::thread th_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false, have_req_ = false;
  struct Req {
    double lat, lon, alt, hdg, ground;
    RidgeParams rp;
  } req_{};
  std::shared_ptr<const TerrainLines> out_;
  std::string status_;
  std::vector<Dem::Near> nearby_;
};

}  // namespace birdie::terrain
