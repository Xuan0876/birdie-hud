"""Camera model for ChasePlane-driven views: world point -> pixel, from logged aircraft + ChasePlane vars.

Conventions (verified on the ground at MMOX, 2026-10-31, calib/cp_views 01-12):
  * camera position in the aircraft body frame, metres (x right, y up, z forward):
      L:p42_cp_mode == 0 (cockpit / cabin):  L:p42_cp_a_x/y/z   (= pilot eyepoint + r)
      L:p42_cp_mode == 1 (external):         L:p42_cp_r_x/y/z   (relative to the aircraft datum; a is a fixed anchor)
    External views only match when the aircraft is level: in turns ChasePlane orients them in a smoothed,
    horizon-level frame that its vars do not expose. Decision 2026-10-31: anchored VFX only in mode-0 shots.
  * L:p42_cp_r_pitch/yaw/roll  camera rotation relative to the aircraft, degrees (pitch + = up, yaw + = right)
  * L:p42_cp_r_zoom   MSFS zoom; focal length in pixels = zoom * F_PER_ZOOM at 3440 px wide
    (cockpit 0.497, cabin 0.630 and external 0.600 all land landmarks / wingtips on target)
  * SimConnect attitude signs (checked in flight, session 20261031_230144): PLANE PITCH DEGREES is negative
    nose-up, PLANE BANK DEGREES is positive left-wing-down; both are flipped in aircraft_rotation()
F_PER_ZOOM comes from fov_calibrate.py: zoom 0.497 -> f 1298 px  (hFOV 106° on a 3440x1440 canvas).
"""
import math

import numpy as np

R_EARTH = 6378137.0
FT = 0.3048
F_PER_ZOOM = 1298.0 / 0.49725


def enu(lat0, lon0, lat, lon):
    e = math.radians(lon - lon0) * R_EARTH * math.cos(math.radians(lat0))
    n = math.radians(lat - lat0) * R_EARTH
    return e, n


def aircraft_to_enu(heading_true, pitch_up, bank_right):
    """Body (x right, y up, z fwd) -> ENU rotation matrix (columns = body axes in ENU)."""
    cy, sy = math.cos(math.radians(heading_true)), math.sin(math.radians(heading_true))
    cp, sp = math.cos(math.radians(pitch_up)), math.sin(math.radians(pitch_up))
    cr, sr = math.cos(math.radians(bank_right)), math.sin(math.radians(bank_right))
    fwd = np.array([sy * cp, cy * cp, sp])
    right0 = np.array([cy, -sy, 0.0])
    up0 = np.cross(right0, fwd)
    right = cr * right0 - sr * up0
    up = sr * right0 + cr * up0
    return np.column_stack([right, up, fwd])


def aircraft_rotation(s):
    """Body -> ENU for a logged/sampled state dict, converting SimConnect attitude signs."""
    return aircraft_to_enu(s["hdg_true"], -s["pitch_deg"], -s["bank_deg"])


def camera_to_body(yaw_right, pitch_up, roll=0.0):
    """Camera -> body rotation, both frames x right / y up / z forward."""
    cy, sy = math.cos(math.radians(yaw_right)), math.sin(math.radians(yaw_right))
    cp, sp = math.cos(math.radians(pitch_up)), math.sin(math.radians(pitch_up))
    fwd = np.array([sy * cp, sp, cy * cp])
    right = np.array([cy, 0.0, -sy])
    up = np.cross(fwd, right)
    if roll:
        cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
        right, up = cr * right - sr * up, sr * right + cr * up
    return np.column_stack([right, up, fwd])


class Camera:
    def __init__(self, s, width=3440, height=1440, f_per_zoom=F_PER_ZOOM):
        """s: dict with lat, lon, alt_ft, hdg_true, pitch_deg (SimConnect sign), bank_deg, cp_* vars."""
        self.lat0, self.lon0 = s["lat"], s["lon"]
        self.W, self.H = width, height
        self.f = s["cp_zoom"] * f_per_zoom * width / 3440.0
        R_ac = aircraft_rotation(s)
        self.R_ac = R_ac
        if s.get("cp_mode", 0) >= 0.5:
            eye = np.array([s["cp_rx"], s["cp_ry"], s["cp_rz"]])
        else:
            eye = np.array([s["cp_eye_x"], s["cp_eye_y"], s["cp_eye_z"]])
        self.pos = np.array([0.0, 0.0, s["alt_ft"] * FT]) + R_ac @ eye
        self.R = R_ac @ camera_to_body(s["cp_yaw"], s["cp_pitch"], s.get("cp_roll", 0.0))

    def project(self, lat, lon, alt_m):
        """Pixel (x, y) or None if behind the camera."""
        e, n = enu(self.lat0, self.lon0, lat, lon)
        v = self.R.T @ (np.array([e, n, alt_m]) - self.pos)
        if v[2] <= 1:
            return None
        return self.W / 2 + self.f * v[0] / v[2], self.H / 2 - self.f * v[1] / v[2]


def from_capture(vals):
    """Adapter for cp_calib_capture.py json (raw simvar names)."""
    return {"lat": vals["PLANE LATITUDE"], "lon": vals["PLANE LONGITUDE"], "alt_ft": vals["PLANE ALTITUDE"],
            "hdg_true": vals["PLANE HEADING DEGREES TRUE"], "pitch_deg": vals["PLANE PITCH DEGREES"],
            "bank_deg": vals["PLANE BANK DEGREES"], "cp_zoom": vals["L:p42_cp_r_zoom"],
            "cp_mode": vals["L:p42_cp_mode"], "cp_rx": vals["L:p42_cp_r_x"], "cp_ry": vals["L:p42_cp_r_y"],
            "cp_rz": vals["L:p42_cp_r_z"],
            "cp_eye_x": vals["L:p42_cp_a_x"], "cp_eye_y": vals["L:p42_cp_a_y"], "cp_eye_z": vals["L:p42_cp_a_z"],
            "cp_yaw": vals["L:p42_cp_r_yaw"], "cp_pitch": vals["L:p42_cp_r_pitch"], "cp_roll": vals["L:p42_cp_r_roll"]}
