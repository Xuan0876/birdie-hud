// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Where the HUD's camera comes from. Everything that projects onto the screen reads the cam_* fields of SimState
// (state.h); a source fills them for each sample:
//   ChasePlane  its L-vars (cp_*): the head angles, the zoom, the eye offset
//   MSFS        the sim's own Camera API (SimConnect_CameraGet, SDK 1.8+, beta), read only: Birdie never acquires
//               or moves the camera
#pragma once

#include <string_view>

#include "core/state.h"

namespace birdie {

enum class CameraSource { kChasePlane = 0, kMsfs = 1 };

// settings.json "camera_source": "chaseplane" | "msfs"; anything else = ChasePlane.
CameraSource camera_source_from(std::string_view name);
const char* camera_source_name(CameraSource src);

// One reply of SimConnect_CameraGet in the SIMOBJECT referential (the user aircraft), as received.
struct MsfsCamera {
  double x = 0, y = 0, z = 0;           // position, m
  double pitch = 0, bank = 0, heading = 0;  // SIMCONNECT_DATA_PBH as named in the header, degrees
  double fov_rad = 0;
};

// The camera from ChasePlane's L-vars. Kept exactly as Birdie computed it before the sources existed (the parity
// tests rely on it): ok = zoom > 0.05, cockpit = mode < 0.5, field of view from the zoom (geo.h vfov_deg), eye from
// the anchor and the view offset (geo.h eye_lateral_m).
void camera_from_chaseplane(SimState& s);

// The camera from the sim's Camera API; cam == nullptr = nothing usable (not supported, no reply yet, an error).
// cockpit comes from CAMERA STATE (2 = cockpit).
void camera_from_msfs(SimState& s, const MsfsCamera* cam);

// Either one.
void apply_camera(SimState& s, CameraSource src, const MsfsCamera* msfs);

// Focal length in px for a vertical field of view (degrees) and a client height.
double focal_from_vfov(double vfov_deg, double height);

}  // namespace birdie
