// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "sim/sim_client.h"

#include <SimConnect.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "core/geo.h"

namespace birdie::sim {
namespace {

enum : DWORD {
  kDefState = 1, kReqState = 1, kDefAircraft = 2, kReqAircraft = 2,
  kDefRunway = 9001, kReqAirports = 9100, kReqRunwayBase = 9200,
  kEvtPause = 1  // system event ids
};

std::string str256(const char* p) {
  std::string s(p, strnlen(p, 256));
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

const char* const kRunwayFields[] = {"LATITUDE",         "LONGITUDE",          "ALTITUDE",
                                     "HEADING",          "LENGTH",             "WIDTH",
                                     "PRIMARY_NUMBER",   "PRIMARY_DESIGNATOR", "SECONDARY_NUMBER",
                                     "SECONDARY_DESIGNATOR", "TRUE_SLOPE"};

#pragma pack(push, 1)
struct RunwayData {  // field order of kRunwayFields
  double lat, lon, alt;
  float heading, length, width;
  int pn, pd, sn, sd;
  float slope;
};
#pragma pack(pop)
static_assert(sizeof(RunwayData) == 56, "runway facility layout");

std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

}  // namespace

bool load_simconnect_dll(std::wstring* where) {
  // Birdie.exe links SimConnect.dll with /DELAYLOAD: load it here so a missing DLL is a message, not a crash.
  if (HMODULE m = GetModuleHandleW(L"SimConnect.dll")) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(m, p, MAX_PATH);
    if (where) *where = p;
    return true;
  }
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
  std::vector<std::wstring> tries{dir + L"SimConnect.dll"};
  wchar_t env[MAX_PATH];
  if (GetEnvironmentVariableW(L"SIMCONNECT_DLL", env, MAX_PATH)) tries.insert(tries.begin(), env);
  for (const auto& p : tries) {
    if (LoadLibraryW(p.c_str())) {
      if (where) *where = p;
      return true;
    }
  }
  return false;
}

bool SimClient::open(HANDLE signal, std::wstring* err) {
  close();
  HANDLE h = nullptr;
  HRESULT hr = SimConnect_Open(&h, "Birdie", nullptr, 0, signal, 0);
  if (FAILED(hr) || !h) {
    if (err) *err = L"SimConnect_Open failed";
    return false;
  }
  h_ = h;
  for (const auto& v : kVars)
    SimConnect_AddToDataDefinition(h_, kDefState, v.simvar, v.unit, SIMCONNECT_DATATYPE_FLOAT64);
  SimConnect_RequestDataOnSimObject(h_, kReqState, kDefState, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SIM_FRAME);
  SimConnect_AddToDataDefinition(h_, kDefAircraft, "TITLE", nullptr, SIMCONNECT_DATATYPE_STRING256);
  SimConnect_AddToDataDefinition(h_, kDefAircraft, "ATC MODEL", nullptr, SIMCONNECT_DATATYPE_STRING256);
  SimConnect_RequestDataOnSimObject(h_, kReqAircraft, kDefAircraft, SIMCONNECT_OBJECT_ID_USER,
                                    SIMCONNECT_PERIOD_SECOND, SIMCONNECT_DATA_REQUEST_FLAG_CHANGED);
  // the ESC menu pauses the sim but the per-frame data keeps coming: only this event tells
  SimConnect_SubscribeToSystemEvent(h_, kEvtPause, "Pause_EX1");
  cam_pending_ = false;
  cam_.reset();
  if (cam_on_) SimConnect_SubscribeToCameraStatusUpdate(h_), SimConnect_CameraGetStatus(h_);  // updates come on change only

  SimConnect_AddToFacilityDefinition(h_, kDefRunway, "OPEN AIRPORT");
  SimConnect_AddToFacilityDefinition(h_, kDefRunway, "OPEN RUNWAY");
  for (const char* f : kRunwayFields) SimConnect_AddToFacilityDefinition(h_, kDefRunway, f);
  SimConnect_AddToFacilityDefinition(h_, kDefRunway, "CLOSE RUNWAY");
  SimConnect_AddToFacilityDefinition(h_, kDefRunway, "CLOSE AIRPORT");
  airports_.clear();
  ends_.clear();
  pending_.clear();
  next_req_ = kReqRunwayBase;
  return true;
}

void SimClient::close() {
  if (h_) SimConnect_Close(h_);
  h_ = nullptr;
  cam_pending_ = false;
  cam_.reset();
  airports_.clear();
  ends_.clear();
  pending_.clear();
}

bool SimClient::dispatch(const Events& ev) {
  if (!h_) return false;
  SIMCONNECT_RECV* msg = nullptr;
  DWORD size = 0;
  latest_.reset();
  for (int guard = 0; guard < 1000; ++guard) {
    HRESULT hr = SimConnect_GetNextDispatch(h_, &msg, &size);
    if (FAILED(hr) || !msg) break;  // E_FAIL = queue empty
    handle(msg, size, ev);
    if (!h_) return false;
  }
  if (latest_ && cam_on_) poll_camera(ev);  // one camera request per state frame
  if (latest_ && ev.state) {
    const SimState s = *latest_;
    latest_.reset();
    ev.state(s);
  }
  return h_ != nullptr;
}

void SimClient::set_camera_polling(bool on) {
  if (on == cam_on_) return;
  cam_on_ = on;
  cam_pending_ = false;
  cam_.reset();
  cam_info_ = CameraInfo{};
  if (!h_) return;
  if (on)
    SimConnect_SubscribeToCameraStatusUpdate(h_), SimConnect_CameraGetStatus(h_);  // updates come on change only
  else
    SimConnect_UnsubscribeToCameraStatusUpdate(h_);
}

void SimClient::set_camera_world(bool on) {
  if (on == world_on_) return;
  world_on_ = on;
  world_pending_ = false;
  world_.reset();
}

const MsfsWorld* SimClient::camera_world() const {
  if (!cam_on_ || !world_on_ || !world_ || !cam_) return nullptr;
  const unsigned long long now = GetTickCount64();
  if (now - world_reply_ms_ > 500 || (world_reply_ms_ > cam_reply_ms_ ? world_reply_ms_ - cam_reply_ms_
                                                                       : cam_reply_ms_ - world_reply_ms_) > 20)
    return nullptr;
  return &*world_;
}

const MsfsCamera* SimClient::camera() const {
  if (!cam_on_ || !cam_ || GetTickCount64() - cam_reply_ms_ > 500) return nullptr;
  return &*cam_;
}

void SimClient::poll_camera(const Events& ev) {
  const unsigned long long now = GetTickCount64();
  if (cam_pending_) {
    if (now - cam_sent_ms_ < 500) return;  // still waiting
    cam_pending_ = false;                  // no answer: ask again (a reply after this is still taken)
    ++cam_info_.errors;
    cam_info_.last_error = "no reply";
  }
  if (now < cam_retry_ms_) return;
  if (FAILED(SimConnect_CameraGet(h_, SIMCONNECT_POSITION_REFERENTIAL_SIMOBJECT))) {
    ++cam_info_.errors;
    cam_info_.last_error = "CameraGet failed";
    cam_retry_ms_ = now + 1000;
    if (ev.log && now - cam_logged_ms_ > 30000) ev.log("[cam] SimConnect_CameraGet failed"), cam_logged_ms_ = now;
    return;
  }
  DWORD id = 0;
  SimConnect_GetLastSentPacketID(h_, &id);
  cam_send_id_ = id;
  cam_sent_ms_ = now;
  cam_pending_ = true;
  // right behind it, so both replies describe the same frame
  if (world_on_ && SUCCEEDED(SimConnect_CameraGet(h_, SIMCONNECT_POSITION_REFERENTIAL_WORLD))) {
    SimConnect_GetLastSentPacketID(h_, &id);
    world_send_id_ = id;
    world_pending_ = true;
  }
}

void SimClient::handle(const void* p, DWORD size, const Events& ev) {
  const auto* r = static_cast<const SIMCONNECT_RECV*>(p);
  switch (r->dwID) {
    case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
      const auto* d = static_cast<const SIMCONNECT_RECV_SIMOBJECT_DATA*>(p);
      if (d->dwRequestID == kReqAircraft) {
        if (size < offsetof(SIMCONNECT_RECV_SIMOBJECT_DATA, dwData) + 512) return;
        const char* b = reinterpret_cast<const char*>(&d->dwData);
        std::string model = str256(b + 256);  // default aircraft: "ATCCOM.AC_MODEL_A20N.0.text" -> "A20N"
        if (model.rfind("ATCCOM.AC_MODEL_", 0) == 0) model = model.substr(16, model.find('.', 16) - 16);
        if (ev.aircraft) ev.aircraft(str256(b), model);
        return;
      }
      if (d->dwRequestID != kReqState) return;
      const size_t need = offsetof(SIMCONNECT_RECV_SIMOBJECT_DATA, dwData) + kVars.size() * sizeof(double);
      if (size < need) return;
      const auto* vals = reinterpret_cast<const double*>(&d->dwData);
      SimState s;
      for (size_t i = 0; i < kVars.size(); ++i) {
        double v;
        std::memcpy(&v, vals + i, sizeof v);
        s.*kVars[i].field = v;
      }
      s.ra_raw_ft = s.radio_alt_ft;
      latest_ = s;  // handed over at the end of dispatch()
      ++samples_;
      return;
    }
    case SIMCONNECT_RECV_ID_EVENT: {
      const auto* e = static_cast<const SIMCONNECT_RECV_EVENT*>(p);
      if (e->uEventID != kEvtPause) return;
      if (ev.log) ev.log(fmt("[sim] pause flags = 0x%lX", e->dwData));
      if (ev.pause) ev.pause(e->dwData);
      return;
    }
    case SIMCONNECT_RECV_ID_QUIT:
      close();
      if (ev.quit) ev.quit();
      return;
    case SIMCONNECT_RECV_ID_CAMERA_DATA: {
      if (size < sizeof(SIMCONNECT_RECV_CAMERA_DATA)) return;
      const auto* d = static_cast<const SIMCONNECT_RECV_CAMERA_DATA*>(p);
      const SIMCONNECT_DATA_CAMERA& c = d->CameraData;
      if (cam_on_ && world_on_ && c.PositionReferential == SIMCONNECT_POSITION_REFERENTIAL_WORLD) {
        world_ = MsfsWorld{c.Position.x, c.Position.y, c.Position.z};  // latitude, longitude, altitude (m)
        world_reply_ms_ = GetTickCount64();
        world_pending_ = false;
        return;
      }
      if (!cam_on_ || c.PositionReferential != SIMCONNECT_POSITION_REFERENTIAL_SIMOBJECT) return;
      MsfsCamera m;
      m.x = c.Position.x, m.y = c.Position.y, m.z = c.Position.z;
      m.pitch = c.Pbh.Pitch, m.bank = c.Pbh.Bank, m.heading = c.Pbh.Heading;
      m.fov_rad = c.Fov;
      const bool first = cam_info_.replies == 0;
      cam_ = m;
      cam_reply_ms_ = GetTickCount64();
      cam_pending_ = false;
      ++cam_info_.replies;
      cam_info_.last_error.clear();
      if (first && ev.log)
        ev.log(fmt("[cam] first camera reply: pos %.2f %.2f %.2f  pbh %.1f %.1f %.1f  fov %.2f deg", m.x, m.y, m.z,
                   m.pitch, m.bank, m.heading, m.fov_rad * 57.29577951308232));
      return;
    }
    case SIMCONNECT_RECV_ID_CAMERA_STATUS: {
      if (size < sizeof(SIMCONNECT_RECV_CAMERA_STATUS)) return;
      const auto* st = static_cast<const SIMCONNECT_RECV_CAMERA_STATUS*>(p);
      const int a = static_cast<int>(st->acquiredState);
      const bool g = st->bGameControlled != 0;
      if (ev.log && (a != cam_info_.availability || g != cam_info_.game_controlled))
        ev.log(fmt("[cam] status: availability %d, game controlled %d", a, g ? 1 : 0));
      cam_info_.availability = a;
      cam_info_.game_controlled = g;
      return;
    }
    case SIMCONNECT_RECV_ID_EXCEPTION: {
      const auto* e = static_cast<const SIMCONNECT_RECV_EXCEPTION*>(p);
      if (world_pending_ && e->dwSendID == world_send_id_) {
        world_pending_ = false;
        return;
      }
      if (cam_on_ && cam_pending_ && e->dwSendID == cam_send_id_) {
        // the camera request failed (no Camera API in this sim, or it said no): try again in a second, log rarely
        cam_pending_ = false;
        ++cam_info_.errors;
        cam_info_.last_error = fmt("exception %lu", e->dwException);
        const unsigned long long now = GetTickCount64();
        cam_retry_ms_ = now + 1000;
        if (ev.log && now - cam_logged_ms_ > 30000) {
          ev.log(fmt("[cam] CameraGet exception %lu (%u so far)", e->dwException, cam_info_.errors));
          cam_logged_ms_ = now;
        }
        return;
      }
      if (ev.log) ev.log(fmt("[sim] exception %lu (send %lu, index %lu)", e->dwException, e->dwSendID, e->dwIndex));
      return;
    }
    case SIMCONNECT_RECV_ID_AIRPORT_LIST: {
      const auto* l = static_cast<const SIMCONNECT_RECV_AIRPORT_LIST*>(p);
      if (l->dwRequestID != kReqAirports) return;
      const size_t base = offsetof(SIMCONNECT_RECV_AIRPORT_LIST, rgData);
      for (DWORD i = 0; i < l->dwArraySize; ++i) {
        if (base + (i + 1) * sizeof(SIMCONNECT_DATA_FACILITY_AIRPORT) > size) break;
        SIMCONNECT_DATA_FACILITY_AIRPORT a;
        std::memcpy(&a, reinterpret_cast<const char*>(p) + base + i * sizeof a, sizeof a);
        char ident[10] = {};
        std::memcpy(ident, a.Ident, 9);
        std::string id(ident);
        while (!id.empty() && id.back() == ' ') id.pop_back();
        if (!id.empty()) airports_[id] = {a.Latitude, a.Longitude, a.Altitude};
      }
      return;
    }
    case SIMCONNECT_RECV_ID_FACILITY_DATA: {
      const auto* f = static_cast<const SIMCONNECT_RECV_FACILITY_DATA*>(p);
      auto it = pending_.find(f->UserRequestId);
      if (it == pending_.end()) return;
      const size_t off = offsetof(SIMCONNECT_RECV_FACILITY_DATA, Data);
      if (f->Type == SIMCONNECT_FACILITY_DATA_RUNWAY && size >= off + sizeof(RunwayData)) {
        RunwayData rd;
        std::memcpy(&rd, reinterpret_cast<const char*>(p) + off, sizeof rd);
        const RunwayRecord rec{rd.lat, rd.lon, rd.alt, rd.heading, rd.length, rd.width,
                               rd.pn,  rd.pd,  rd.sn,  rd.sd,      rd.slope};
        auto ends = ends_from_record(it->second, rec);
        if (ends.empty() && ev.log)
          ev.log(fmt("[rwy] %s: implausible runway record skipped (hdg %.1f len %.0f wid %.0f)", it->second.c_str(),
                     rd.heading, rd.length, rd.width));
        for (auto& e : ends) ends_[it->second].push_back(std::move(e));
      }
      return;
    }
    case SIMCONNECT_RECV_ID_FACILITY_DATA_END: {
      const auto* e = static_cast<const SIMCONNECT_RECV_FACILITY_DATA_END*>(p);
      auto it = pending_.find(e->RequestId);
      if (it == pending_.end()) return;
      if (ev.log) {
        std::string names;
        for (const auto& r2 : ends_[it->second]) names += (names.empty() ? "" : ", ") + r2.name;
        ev.log("[rwy] " + it->second + ": " + (names.empty() ? std::string("no runway data") : names));
      }
      pending_.erase(it);
      return;
    }
    default:
      return;
  }
}

void SimClient::request_airports() {
  if (h_) SimConnect_RequestFacilitiesList_EX1(h_, SIMCONNECT_FACILITY_LIST_TYPE_AIRPORT, kReqAirports);
}

void SimClient::request_runways(const std::string& icao) {
  if (!h_ || ends_.count(icao)) return;
  for (const auto& [id, name] : pending_)
    if (name == icao) return;
  const unsigned long rid = next_req_++;
  pending_[rid] = icao;
  ends_[icao];  // empty until the data arrives: requested once
  SimConnect_RequestFacilityData(h_, kDefRunway, rid, icao.c_str(), "");
}

std::vector<std::string> SimClient::nearby(double lat, double lon, double radius) const {
  std::vector<std::string> out;
  for (const auto& [icao, a] : airports_) {
    const Vec2 en = enu(lat, lon, a.lat, a.lon);
    if (en.x * en.x + en.y * en.y < radius * radius) out.push_back(icao);
  }
  return out;
}

std::vector<const Runway*> SimClient::all_ends(const std::vector<std::string>& icaos) const {
  std::vector<const Runway*> out;
  for (const auto& i : icaos) {
    auto it = ends_.find(i);
    if (it == ends_.end()) continue;
    for (const auto& r : it->second) out.push_back(&r);
  }
  return out;
}

std::vector<Runway*> SimClient::airport_ends(const std::string& icao) {
  std::vector<Runway*> out;
  if (auto it = ends_.find(icao); it != ends_.end())
    for (auto& r : it->second) out.push_back(&r);
  return out;
}

void SimClient::anchor_runways(const std::vector<std::string>& icaos, double lat, double lon, double ground_m,
                               const std::function<void(const std::string&)>& log) {
  for (const auto& i : icaos) {
    auto it = ends_.find(i);
    if (it == ends_.end()) continue;
    for (auto& r : it->second) {
      const bool first = !r.anchored;
      if (anchor_from_ground(r, lat, lon, ground_m) && first && log) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "[rwy] %s %s surface from the ground: centre %.1f m (facility %.1f, %+.1f m)",
                      r.icao.c_str(), r.name.c_str(), r.alt_m, r.facility_alt_m, r.alt_m - r.facility_alt_m);
        log(buf);
      }
    }
  }
}

}  // namespace birdie::sim
