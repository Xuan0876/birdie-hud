// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Where the HUD's camera comes from. Everything that projects onto the screen reads the cam_* fields of SimState
// (state.h); a source fills them for each sample:
//   ChasePlane  its L-vars (cp_*): the head angles, the zoom, the eye offset
//   MSFS        the sim's own Camera API (SimConnect_CameraGet, SDK 1.8+, beta), read only: Birdie never acquires
//               or moves the camera
#pragma once

#include <string_view>

#include "core/state.h"
#include "core/vec.h"

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

// One reply of SimConnect_CameraGet in the WORLD referential: where the camera is.
struct MsfsWorld {
  double lat = 0, lon = 0;  // degrees
  double alt_m = 0;         // metres above MSL
};

// The SIMOBJECT referential has its own origin, not the point PLANE LATITUDE / LONGITUDE / ALTITUDE describe (the
// one Birdie places the aircraft at). Measured 11/24 on the FSLabs A321neo (probe, every view alike): same lateral and
// vertical, but 21.34 m further forward, so the eye read as is sat 21 m too far back and the runway's near end was
// drawn too high. The offset is per aircraft: Birdie measures it while the aircraft stands still (then the WORLD
// position, the SIMOBJECT one and the state belong to the same instant) and keeps it per aircraft.
// Returns the offset in Birdie's body frame (x right, y up, z forward, m): eye = SIMOBJECT eye + offset.
Vec3 msfs_origin_offset(const SimState& s, const MsfsCamera& rel, const MsfsWorld& world);

// An origin offset Birdie accepts: |x|, |y| <= 3 m, |z| <= 60 m (measured: FSLabs A321 +21.4 m along, C152 +2.2 m).
// 11/24 the measurement took frames while the sim was loading or in its menus (the position "stands still" there
// too) and kept offsets of millions of metres; the eye then sat far to the right, the left seat read as the right
// one, and a border traced from the captain's seat was stored as the first officer's.
bool plausible_origin(const Vec3& o);
// An eye Birdie can place a seat and a border with: finite, |x| <= 3 m, |y| <= 10 m, |z| <= 80 m from the reference.
bool plausible_eye(const SimState& s);

// The camera from ChasePlane's L-vars. Kept exactly as Birdie computed it before the sources existed (the parity
// tests rely on it): ok = zoom > 0.05, cockpit = mode < 0.5, field of view from the zoom (geo.h vfov_deg), eye from
// the anchor and the view offset (geo.h eye_lateral_m).
void camera_from_chaseplane(SimState& s);

// The camera from the sim's Camera API; cam == nullptr = nothing usable (not supported, no reply yet, an error).
// cockpit comes from CAMERA STATE (2 = cockpit). origin = msfs_origin_offset() for this aircraft (0 = not known yet).
void camera_from_msfs(SimState& s, const MsfsCamera* cam, const Vec3& origin = {});

// Either one.
void apply_camera(SimState& s, CameraSource src, const MsfsCamera* msfs, const Vec3& msfs_origin = {});

// Focal length in px for a vertical field of view (degrees) and a client height.
double focal_from_vfov(double vfov_deg, double height);

}  // namespace birdie
