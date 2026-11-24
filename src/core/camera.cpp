// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "core/camera.h"

#include <cmath>

#include "core/geo.h"

namespace birdie {

CameraSource camera_source_from(std::string_view name) {
  return name == "msfs" ? CameraSource::kMsfs : CameraSource::kChasePlane;
}

const char* camera_source_name(CameraSource src) { return src == CameraSource::kMsfs ? "msfs" : "chaseplane"; }

void camera_from_chaseplane(SimState& s) {
  s.cam_source = static_cast<int>(CameraSource::kChasePlane);
  s.cam_ok = s.cp_zoom > 0.05;
  s.cam_cockpit = s.cp_mode < 0.5;
  s.cam_yaw = s.cp_yaw;
  s.cam_pitch = s.cp_pitch;
  s.cam_roll = s.cp_roll;
  s.cam_vfov = vfov_deg(s.cp_zoom);
  s.cam_eye_x = eye_lateral_m(s.cp_eye_x, s.cp_off_x);
  s.cam_eye_y = s.cp_eye_y;
  s.cam_eye_z = s.cp_eye_z;
}

// The Camera API's frame, from the 11/24 read-only probe (birdie-notes tools/camera_api_probe; to be confirmed in
// the sim, docs CAMERA_SOURCE_PLAN.md V1-V4):
//  - position: z forward (the chase view sits at z -57 m), y up, x + = LEFT: the captain's default view reads
//    x +0.53 m (ChasePlane's anchor is + = left too). Birdie's eye is + = right, so x is flipped.
//  - the field named Bank turns with the head (+ = right: an orbit on the + x side looks back with Bank about +100°),
//    the one named Heading stays about 0 (taken as the roll), Pitch + = looking down.
//  - Fov: the default cockpit view reads 58.18°, ChasePlane's default vertical field of view is 58.0° (zoom_calib,
//    on a 21:9 screen, where the horizontal one would be about 105°): taken as vertical.
void camera_from_msfs(SimState& s, const MsfsCamera* cam) {
  s.cam_source = static_cast<int>(CameraSource::kMsfs);
  s.cam_cockpit = std::lround(s.cam_state) == 2;
  if (!cam || !(cam->fov_rad > 0) || !std::isfinite(cam->fov_rad)) {
    s.cam_ok = false;
    s.cam_yaw = s.cam_pitch = s.cam_roll = 0;
    s.cam_vfov = 60;  // anything finite: the HUD is hidden (hide_reason) but helpers may still build a frame
    s.cam_eye_x = s.cam_eye_y = s.cam_eye_z = 0;
    return;
  }
  s.cam_ok = true;
  s.cam_yaw = cam->bank;
  s.cam_pitch = -cam->pitch;
  s.cam_roll = cam->heading;
  s.cam_vfov = deg(cam->fov_rad);
  s.cam_eye_x = -cam->x;
  s.cam_eye_y = cam->y;
  s.cam_eye_z = cam->z;
}

void apply_camera(SimState& s, CameraSource src, const MsfsCamera* msfs) {
  if (src == CameraSource::kMsfs)
    camera_from_msfs(s, msfs);
  else
    camera_from_chaseplane(s);
}

double focal_from_vfov(double vfov, double height) { return height / 2 / std::tan(rad(vfov / 2)); }

}  // namespace birdie
