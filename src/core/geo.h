// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Camera model for ChasePlane-driven views (reference: cp_camera.py, docs/REFERENCE.md).
//   Frames: ENU (east, north, up), body and camera (x right, y up, z forward).
//   SimConnect attitude: pitch negative = nose up, bank positive = left wing down (flipped here).
#pragma once

#include "core/state.h"
#include "core/vec.h"

namespace birdie {

constexpr double kEarthRadius = 6378137.0;
constexpr double kFt = 0.3048;
constexpr double kKt = 0.514444;
// ChasePlane zoom -> field of view. Measured 2026-11-15 (tools/zoom_calib, C152, 3440x1440, zoom 0.12-0.83): the
// VERTICAL field of view is linear in the zoom, -113.319° per unit (all points within 0.25 % in focal length).
// Anchored at the video-verified point zoom 0.49725 -> 1298 px focal at 1440 px client height (A321 FOE check).
// The Python reference used f = zoom * 1298/0.49725 * width/3440 (right only near 0.497 and at 21:9).
constexpr double kZoomRef = 0.49725;
constexpr double kFocalRefPx = 1298.0;      // at kZoomRef, 1440 px client height
constexpr double kVfovPerZoom = -113.319;   // deg per unit of zoom

// Earth model for lat/lon <-> local metres. MSFS places everything on the WGS84 ellipsoid (measured 2026-11-15 with
// tools/runway_calib/forward.py, 15 runways at 9 airports: the painted runway sits on the facility centreline within
// ~0.2 m once the ellipsoid's local radii are used). A sphere of radius kEarthRadius makes north 0.3 % too long and
// east 0.1 % too short at 35°, which turns every bearing by up to 0.13° — 8 m sideways at 4 km on final (ZSNB 31).
//
// Reference mode = the frozen Python reference's behaviour, for the parity tests only: the sphere above, and
// ChasePlane's L:p42_cp_a_x taken as the eye's lateral offset as is (see eye_lateral_m).
void set_reference_mode(bool on);
bool reference_mode();

// Eye (camera) lateral offset from the aircraft datum, metres, + = right, from ChasePlane's L-vars.
// L:p42_cp_a_x = anchor + L:p42_cp_r_x, where the anchor is the aircraft's eyepoint with + = LEFT and r_x the view's
// offset from it with + = right (FBW A320, 11/15: pilot a_x +0.548 = 0.567 - 0.019, copilot a_x +1.737 = 0.567 +
// 1.170). So the eye is at r_x - anchor = 2 r_x - a_x: pilot -0.59 m, copilot +0.60 m. Using a_x as is put the eye
// 1.1 m too far right; measured from both directions on 6 runways (tools/runway_calib/forward.py) the picture
// runway was 0.9-1.0 m off at every range (pilot and copilot alike); with 2 r_x - a_x about 0.2 m.
double eye_lateral_m(double a_x, double r_x);

// Local flat east/north offset (m) of (lat, lon) from (lat0, lon0), radii of curvature at lat0.
Vec2 enu(double lat0, double lon0, double lat, double lon);
// (lat, lon) moved by east/north metres.
Vec2 offset_latlon(double lat, double lon, double e, double n);  // returns {lat, lon}

// Body -> ENU rotation (columns = body axes in ENU), pitch up / bank right positive.
Mat3 aircraft_to_enu(double heading_true, double pitch_up, double bank_right);
// Body -> ENU for a SimConnect state (sign conversion included).
Mat3 aircraft_rotation(const SimState& s);
// Camera -> body rotation from ChasePlane head angles (yaw right, pitch up, roll).
Mat3 camera_to_body(double yaw_right, double pitch_up, double roll = 0.0);
// Focal length in client pixels for a ChasePlane zoom and client height (MSFS fixes the vertical field of view).
double focal_px(double zoom, double height);
// Vertical field of view, degrees, for a ChasePlane zoom.
double vfov_deg(double zoom);

}  // namespace birdie
