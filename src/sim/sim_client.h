// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// SimConnect connection: the per-frame state definition (core/state.h kVars) and the airport / runway facility
// requests (reference: overlay.py run_live, runways.py FacilityClient).
#pragma once

#include <windows.h>

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/camera.h"
#include "core/runway.h"
#include "core/state.h"

namespace birdie::sim {

// True if SimConnect.dll can be loaded (next to Birdie.exe, or SIMCONNECT_DLL). Must be called before open().
bool load_simconnect_dll(std::wstring* where);

class SimClient {
 public:
  struct Events {
    std::function<void(const SimState&)> state;        // one sample per sim frame (radio_alt_ft = raw RA)
    std::function<void()> quit;                        // the simulator closed
    std::function<void(const std::string&)> log;       // diagnostics
    // user aircraft identity, sent when it changes: TITLE (includes the livery) and ATC MODEL ("A21N")
    std::function<void(const std::string& title, const std::string& model)> aircraft;
    // the Pause_EX1 system event: pause flags (core/pause.h), 0 = running
    std::function<void(unsigned flags)> pause;
  };

  ~SimClient() { close(); }
  // `signal` is set by SimConnect whenever messages are waiting.
  bool open(HANDLE signal, std::wstring* err);
  void close();
  bool connected() const { return h_ != nullptr; }
  // Process every waiting message. Returns false if the connection broke (it is closed then).
  // The per-frame state comes once per call, newest sample only: a slow paint (the GPU busy with the sim) must not
  // let the queue grow, or the HUD falls seconds behind the sim and the backlog only drains when painting speeds up.
  bool dispatch(const Events& ev);
  // State samples received since the last call (including the ones dispatch() skipped as stale).
  int take_samples() {
    const int n = samples_;
    samples_ = 0;
    return n;
  }

  // ---- facilities: airports in the reality bubble, runways fetched once per airport
  void request_airports();
  void request_runways(const std::string& icao);
  std::vector<std::string> nearby(double lat, double lon, double radius_m = 30000.0) const;
  std::vector<const Runway*> all_ends(const std::vector<std::string>& icaos) const;
  // One airport's ends, to correct their heights (core/runway_heights.h); empty until its runways have arrived.
  std::vector<Runway*> airport_ends(const std::string& icao);
  // Re-anchors the altitude of every runway end whose pavement is under (lat, lon); ground_m = terrain/pavement
  // altitude there (PLANE ALTITUDE - PLANE ALT ABOVE GROUND). Logs the first anchoring of each end.
  void anchor_runways(const std::vector<std::string>& icaos, double lat, double lon, double ground_m,
                      const std::function<void(const std::string&)>& log);

  // ---- the sim's own camera (core/camera.h, the MSFS source). Read only: never CameraAcquire / CameraSet.
  // While on, SimConnect_CameraGet(SIMOBJECT) goes out once per state frame, one request at a time.
  struct CameraInfo {
    unsigned replies = 0, errors = 0;  // since it was switched on
    int availability = -1;             // SIMCONNECT_CAMERA_AVAILABILITY from the status updates, -1 = none yet
    bool game_controlled = false;
    std::string last_error;            // "" = none
  };
  void set_camera_polling(bool on);
  bool camera_polling() const { return cam_on_; }
  // The newest reply, or nullptr: off, nothing yet, or no reply for 0.5 s (an error, a sim without the API).
  const MsfsCamera* camera() const;
  // While on (and polling), a CameraGet(WORLD) goes out with each SIMOBJECT one: to measure the SIMOBJECT origin
  // (camera.h msfs_origin_offset). camera_world() = its newest reply if it came within 20 ms of the SIMOBJECT one
  // (the same sim frame) and is fresh, else nullptr.
  void set_camera_world(bool on);
  const MsfsWorld* camera_world() const;
  const CameraInfo& camera_info() const { return cam_info_; }

 private:
  void handle(const void* msg, DWORD size, const Events& ev);
  HANDLE h_ = nullptr;
  std::optional<SimState> latest_;  // newest state of the current dispatch() round
  int samples_ = 0;
  struct Airport {
    double lat, lon, alt;
  };
  std::map<std::string, Airport> airports_;
  std::map<std::string, std::deque<Runway>> ends_;  // deque: pointers stay valid while it grows
  std::map<unsigned long, std::string> pending_;    // request id -> icao
  unsigned long next_req_ = 0;

  void poll_camera(const Events& ev);
  bool cam_on_ = false;
  bool cam_pending_ = false;      // a CameraGet is out
  unsigned long cam_send_id_ = 0;  // its packet id (exceptions name it)
  bool world_on_ = false, world_pending_ = false;
  unsigned long world_send_id_ = 0;
  unsigned long long world_reply_ms_ = 0;
  std::optional<MsfsWorld> world_;
  unsigned long long cam_sent_ms_ = 0, cam_reply_ms_ = 0, cam_retry_ms_ = 0, cam_logged_ms_ = 0;
  std::optional<MsfsCamera> cam_;
  CameraInfo cam_info_;
};

}  // namespace birdie::sim
