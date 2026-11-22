// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The terrain data on disk: where it lives, what is there, how much room it takes, and getting more.
//
// Folder (default %LOCALAPPDATA%\Birdie\dem, the user can move it):
//   <dir>\N34W119.bdem      packages: downloaded on purpose (a region, around a point, or imported); never trimmed
//   <dir>\cache\*.bdem      streaming: fetched by themselves around the aircraft; the least recently used go first
//                           once the cache is over its size limit (never the tiles in range right now)
//   <dir>\import\           drop Copernicus GeoTIFFs (.tif, or the .zip / .tar they came in) here to install them
// Data: Copernicus DEM GLO-30 / GLO-90 from the AWS open data buckets (no account): see THIRD_PARTY_NOTICES.md.
//
// One background thread does all the slow work, one tile at a time: streaming first (needed now), then packages.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "terrain/terrain.h"

namespace birdie::terrain {

struct Region {
  const char* id;
  const char* zh;
  const char* en;
  int south, north, west, east;  // tiles whose south-west corner lies in [south, north) x [west, east)
};
const std::vector<Region>& regions();
// The land tiles of a region / within km of a point.
std::vector<std::string> region_tiles(const Region& r);
std::vector<std::string> tiles_around(double lat, double lon, double km);
// About how large one installed tile is (bytes): the Copernicus tiles get narrower towards the poles.
uint64_t tile_bytes(int ilat, int res);
// %LOCALAPPDATA%\Birdie\dem
std::filesystem::path default_dir();

struct StoreStats {
  std::string dir;
  int pkg_tiles = 0, cache_tiles = 0, import_files = 0;
  uint64_t pkg_bytes = 0, cache_bytes = 0;
  uint64_t disk_free = 0;  // on the folder's drive
};

// What the background thread is doing.
struct StoreProgress {
  std::string kind;   // "" idle, "stream", "package", "import", "move"
  std::string label;  // for "package": the region id or "around"
  int done = 0, total = 0, failed = 0;
  std::string tile;   // the one being fetched
  uint64_t got = 0, size = 0;  // bytes of it so far / in all (0 = unknown)
  double rate = 0;    // bytes/s, smoothed
  std::string error;  // the last thing that went wrong ("" = none)
};

// Something the HUD may say in its status line (app: the captions under the HUD toggle).
struct StoreEvent {
  enum Kind { kStreamStart, kStreamDone, kStreamFailed, kPackageDone, kImportDone, kMoveDone, kMoveFailed } kind;
  int n = 0;
  uint64_t bytes = 0;
  std::string text;
};

class Store {
 public:
  Store();
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  void set_dir(const std::filesystem::path& dir);  // stops whatever runs
  std::filesystem::path dir() const;

  // Streaming: the land tiles in range now, nearest first (TerrainWorker::nearby). The missing ones within reach are
  // fetched into the cache at res (30 / 90 m); all listed ones are kept when the cache is trimmed to cap_bytes. Call
  // every couple of seconds while it should stream; stop_stream() when not (HUD off, terrain off).
  void stream(const std::vector<Dem::Near>& nearby, double reach_km, int res, uint64_t cap_bytes);
  void stop_stream();
  bool streaming() const;

  // Packages. install_*: queue the tiles not installed yet (at res; a 90 m tile is replaced by a 30 m one).
  void install(const std::string& label, const std::vector<std::string>& tiles, int res);
  void stop_packages();
  // Deletes the packages among these tiles (all of them when empty). Returns how many files went.
  int delete_packages(const std::vector<std::string>& tiles);
  // Installed packages among these tiles.
  int count_packages(const std::vector<std::string>& tiles) const;
  int clear_cache();                 // returns how many files went
  void trim_cache(uint64_t cap_bytes);  // now (after the limit was lowered)
  void import_folder();              // converts what is in <dir>\import
  // Moves everything to a new folder (in the background); the store points there afterwards. False = refused
  // (the same folder, inside the old one, not writable).
  bool move_to(const std::filesystem::path& to, std::string* err);

  StoreStats stats();  // recounted at most every 2 s, or after a change
  StoreProgress progress() const;
  std::vector<StoreEvent> take_events();
  // A new tile arrived since the last call (the worker may then redo its lines sooner).
  bool take_changed();

 private:
  struct Job {
    std::string tile;
    int res;
    bool cache;  // streaming (into the cache) or a package
  };
  void run();
  bool fetch(const Job& j, std::string* err);  // download + convert one tile
  bool have(const std::string& tile, int res, bool cache_ok) const;
  void trim_locked(uint64_t cap_bytes, const std::set<std::string>& keep);
  void touch(const std::vector<Dem::Near>& keep);
  void do_import();
  void do_move(std::filesystem::path to);
  void push_event(StoreEvent e);
  void set_error(const std::string& e);

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread th_;
  bool quit_ = false;
  std::filesystem::path dir_;
  // streaming
  bool stream_on_ = false;
  std::vector<Job> want_;           // missing tiles in reach, nearest first
  std::set<std::string> keep_;      // tiles in range (not trimmed)
  uint64_t cap_ = 0;
  int stream_got_ = 0;              // fetched since the stream last went idle
  uint64_t stream_bytes_ = 0;
  bool stream_busy_ = false;
  double t_touch_ = -1e9;
  std::map<std::string, double> failed_at_;  // tile -> when it last failed (s): retried after a while
  double net_wait_until_ = 0;       // no network: wait before the next try
  // packages
  std::deque<Job> pkg_;
  std::string pkg_label_;
  int pkg_done_ = 0, pkg_total_ = 0, pkg_failed_ = 0;
  // one-off jobs
  bool want_import_ = false;
  std::optional<std::filesystem::path> want_move_;
  std::atomic<bool> cancel_{false};  // stops the download in flight
  StoreProgress prog_;
  std::vector<StoreEvent> events_;
  bool changed_ = false;
  StoreStats stats_;
  double t_stats_ = -1e9;
};

}  // namespace birdie::terrain
