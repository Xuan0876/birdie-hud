"""FPV overlay: a click-through, always-on-top transparent window over MSFS that draws the flight path vector
("bird") and a glide-path reference line N degrees below the horizon, from live SimConnect + ChasePlane data.

    python overlay.py                 # live (MSFS borderless fullscreen, ChasePlane cockpit view)
    python overlay.py --snapshot <session_dir> <video_s> [--lag -0.34] [--out x.png]
                                      # offline: draw the overlay on one frame of a recording (checks the drawing)

Hotkeys (global):  Ctrl+Alt+PgUp/PgDn  glide angle ±0.1°   Ctrl+Alt+Home/End  lead ±10 ms
                   Ctrl+Alt+Insert     show / hide         Ctrl+Alt+Shift+End quit
(Ctrl+Alt+arrows and Ctrl+Alt+F are already taken on this PC.)
Settings persist in settings.json next to this file.

Model (validated offline, see PROGRESS.md): camera rotation = aircraft attitude x ChasePlane head angles
(L:p42_cp_r_pitch/yaw/roll), focal = L:p42_cp_r_zoom x 2612 px at 3440 wide, principal point = client centre.
Bird = ground velocity direction: horizontal from GPS GROUND TRUE TRACK + GROUND VELOCITY, vertical from
VELOCITY WORLD Y (VERTICAL SPEED lags in the flare by up to 0.8°).
"""
import argparse
import ctypes
import json
import math
import os
import queue
import struct
import sys
import threading
import time
from ctypes import wintypes

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cp_camera as C  # noqa: E402
import runways as RW  # noqa: E402
import hud as HUD  # noqa: E402

KT = 0.514444
FT = 0.3048
SETTINGS = os.path.join(HERE, "settings.json")
DEFAULTS = {"glide_deg": 3.0, "lead_ms": 0, "smooth_s": 0.15, "box_w": 0.6, "box_h": 0.7, "visible": True,
            "line_span_deg": 18.0, "line_gap_deg": 6.0, "show_runway": True, "show_bird": True,
            "show_glide": True, "show_horizon": True, "opacity": 1.0, "ladder_step_deg": 6.0, "ladder_max_deg": 30.0,
            "show_speed": True, "show_alt": True, "show_vs": True, "show_roll": True, "show_hdg": True,
            "show_gs": True, "show_att": True, "show_fd": True, "hud_scale": 1.0,
            "show_pitch_minor": True, "show_fpa_num": True, "show_vs_num": True, "fd_ra_cutoff": True, "show_flare": True, "flare_tol_deg": 0.3,
            "ra_offset_ft": 0.0, "ra_offset_note": "", "fpd_lag_ms": 120, "fpd_tau_ms": 100}
APPDIR = os.path.join(os.environ.get("APPDATA", HERE), "fpv_overlay")
LOG_PATH = os.path.join(APPDIR, "overlay.log")
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_NAME = "fpv_overlay"
NL = chr(10)


class _Log:
    """print() target: console when there is one (python), log file always (pythonw has no stdout)."""

    def __init__(self, console):
        self.console = console
        os.makedirs(APPDIR, exist_ok=True)
        try:
            if os.path.getsize(LOG_PATH) > 2_000_000:
                os.replace(LOG_PATH, LOG_PATH + ".old")
        except OSError:
            pass
        self.f = open(LOG_PATH, "a", encoding="utf-8", buffering=1)
        self.f.write(NL + "===== " + time.strftime("%Y-%m-%d %H:%M:%S") + " 启动 =====" + NL)
        self.buf = ""

    def write(self, text):
        if self.console:
            try:
                self.console.write(text)
            except (OSError, ValueError, UnicodeEncodeError):
                pass
        self.buf += text
        while NL in self.buf:
            line, self.buf = self.buf.split(NL, 1)
            if line.strip():
                self.f.write(time.strftime("[%H:%M:%S] ") + line + NL)

    def flush(self):
        if self.console:
            try:
                self.console.flush()
            except (OSError, ValueError):
                pass


VARS = [  # (key, simvar, unit)
    ("lat", "PLANE LATITUDE", "degrees"),
    ("lon", "PLANE LONGITUDE", "degrees"),
    ("hdg_true", "PLANE HEADING DEGREES TRUE", "degrees"),
    ("pitch_deg", "PLANE PITCH DEGREES", "degrees"),  # negative = nose up
    ("bank_deg", "PLANE BANK DEGREES", "degrees"),  # positive = left wing down
    ("trk_true", "GPS GROUND TRUE TRACK", "degrees"),
    ("gs_kt", "GROUND VELOCITY", "knots"),
    ("vy_fps", "VELOCITY WORLD Y", "feet per second"),
    ("vs_fpm", "VERTICAL SPEED", "feet per minute"),
    ("alt_ft", "PLANE ALTITUDE", "feet"),
    ("agl_ft", "PLANE ALT ABOVE GROUND", "feet"),
    ("on_ground", "SIM ON GROUND", "bool"),
    ("cp_mode", "L:p42_cp_mode", "number"),
    ("cp_pitch", "L:p42_cp_r_pitch", "number"),
    ("cp_yaw", "L:p42_cp_r_yaw", "number"),
    ("cp_roll", "L:p42_cp_r_roll", "number"),
    ("cp_zoom", "L:p42_cp_r_zoom", "number"),
    ("cp_eye_x", "L:p42_cp_a_x", "number"),  # camera position in the body frame, metres (right/up/fwd)
    ("cp_eye_y", "L:p42_cp_a_y", "number"),
    ("cp_eye_z", "L:p42_cp_a_z", "number"),
    ("ias_kt", "AIRSPEED INDICATED", "knots"),
    ("mach", "AIRSPEED MACH", "mach"),
    ("ind_alt_ft", "INDICATED ALTITUDE", "feet"),
    ("radio_alt_ft", "RADIO HEIGHT", "feet"),
    ("baro_mb", "KOHLSMAN SETTING MB", "millibars"),
    ("hdg_mag", "PLANE HEADING DEGREES MAGNETIC", "degrees"),
    ("wind_dir", "AMBIENT WIND DIRECTION", "degrees"),  # true, from
    ("wind_kt", "AMBIENT WIND VELOCITY", "knots"),
    ("beta_deg", "INCIDENCE BETA", "degrees"),
]
ANGLE_KEYS = ("hdg_true", "pitch_deg", "bank_deg", "cp_pitch", "cp_yaw", "cp_roll")


def load_settings():
    s = dict(DEFAULTS)
    try:
        with open(SETTINGS, encoding="utf-8") as f:
            s.update(json.load(f))
    except (OSError, ValueError):
        pass
    return s


def save_settings(s):
    try:
        with open(SETTINGS, "w", encoding="utf-8") as f:
            json.dump(s, f, ensure_ascii=False, indent=1)
    except OSError:
        pass


# ---------------------------------------------------------------- geometry (no Qt)
class FpdTracker:
    """FSLabs' flight path director arrives at ~15-20 Hz, ~0.1-0.2 s late and pixel-quantised, so drawing the
    raw orders makes the ring step. Each new reading becomes absolute targets (the FPA and bank the director asks
    for = our own FPA / bank at the picture's time + the orders), the targets are eased (tau), and the relative
    orders are re-derived every sim frame from live data: the ring moves smoothly with the aircraft."""

    def __init__(self, settings=None, lag=0.12, tau=0.10):
        from collections import deque
        self.hist = deque(maxlen=150)  # (t, fpa_deg, bank_right_deg)
        self.settings = settings
        self.lag, self.tau = lag, tau  # used without settings (tests); tray: fpd_lag_ms / fpd_tau_ms
        self.last_t = None
        self.raw = None  # latest (fpa, bank) target
        self.cur = None  # eased target
        self.t_apply = None

    def add(self, t, model, s):
        v = model.v
        if v is None:
            return
        fpa = math.degrees(math.atan2(v[2], math.hypot(v[0], v[1])))
        self.hist.append((t, fpa, -s["bank_deg"]))  # SimConnect bank: + = left wing down

    def _at(self, t):
        h = self.hist
        if t <= h[0][0]:
            return h[0][1:]
        for i in range(len(h) - 1, 0, -1):
            if h[i - 1][0] <= t:
                (t0, f0, b0), (t1, f1, b1) = h[i - 1], h[i]
                a = 0.0 if t1 <= t0 else min(1.0, (t - t0) / (t1 - t0))
                return f0 + a * (f1 - f0), b0 + a * (b1 - b0)
        return h[-1][1:]

    def apply(self, fd, now):
        if self.settings is not None:
            self.lag = self.settings.get("fpd_lag_ms", 120) / 1000.0
            self.tau = self.settings.get("fpd_tau_ms", 100) / 1000.0
        if fd is None or fd.get("mode") != "fpa" or fd.get("pitch") is None or not self.hist:
            self.raw = self.cur = None
            return fd
        if fd["t"] != self.last_t:
            self.last_t = fd["t"]
            f0, b0 = self._at(fd["t"] - self.lag)
            self.raw = (f0 + fd["pitch"], b0 + fd["roll"])
            if self.cur is None:
                self.cur = self.raw
        if self.t_apply is not None and self.cur is not None:
            a = 1.0 if self.tau <= 0 else 1.0 - math.exp(-max(0.0, now - self.t_apply) / self.tau)
            self.cur = (self.cur[0] + a * (self.raw[0] - self.cur[0]), self.cur[1] + a * (self.raw[1] - self.cur[1]))
        self.t_apply = now
        _t, fpa, bank = self.hist[-1]
        return dict(fd, pitch=self.cur[0] - fpa, roll=self.cur[1] - bank)


class Model:
    """Turns raw samples into screen geometry. Keeps the velocity low-pass and the attitude rates for the lead."""

    def __init__(self, settings):
        self.set = settings
        self.v = None  # smoothed ENU velocity, m/s
        self.prev = None  # (t, state) for rates
        self.rates = {k: 0.0 for k in ANGLE_KEYS}
        self.filt = HUD.HudFilters()

    def update(self, s, t):
        self.filt.update(s, t)
        trk = math.radians(s["trk_true"])
        gs = s["gs_kt"] * KT
        v = np.array([gs * math.sin(trk), gs * math.cos(trk), s["vy_fps"] * FT])
        if self.v is None or not self.prev or t - self.prev[0] > 0.5:
            self.v = v
        else:
            a = 1.0 - math.exp(-(t - self.prev[0]) / max(self.set["smooth_s"], 1e-3))
            self.v = self.v + a * (v - self.v)
        if self.prev and 0 < t - self.prev[0] < 0.5:
            dt = t - self.prev[0]
            for k in ANGLE_KEYS:
                d = (s[k] - self.prev[1][k] + 180) % 360 - 180
                self.rates[k] += 0.5 * (d / dt - self.rates[k])
        self.prev = (t, dict(s))

    def fpa_deg(self):
        """Flight path angle of the (smoothed) velocity the bird is drawn from, or None."""
        v = self.v
        if v is None or np.linalg.norm(v[:2]) <= 1.0:
            return None
        return math.degrees(math.atan2(v[2], math.hypot(v[0], v[1])))

    def vs_fpm(self):
        """Vertical speed of the same smoothed velocity (ft/min), or None."""
        return None if self.v is None else self.v[2] / FT * 60.0

    def geometry(self, s, W, H, runway=None):
        """Bird pixel (or None), glide-line polylines and runway outline polygon in client pixels (W x H client)."""
        lead = self.set["lead_ms"] / 1000.0
        p = dict(s)
        if lead:
            for k in ANGLE_KEYS:
                p[k] = s[k] + self.rates[k] * lead
        R = C.aircraft_rotation(p) @ C.camera_to_body(p["cp_yaw"], p["cp_pitch"], p["cp_roll"])
        f = p["cp_zoom"] * C.F_PER_ZOOM * W / 3440.0

        def pix(d):
            r = R.T @ d
            if r[2] <= 1e-3:
                return None
            return (W / 2 + f * r[0] / r[2], H / 2 - f * r[1] / r[2])

        v = self.v if self.v is not None else np.zeros(3)
        bird = pix(v) if np.linalg.norm(v[:2]) > 1.0 else None
        g = math.radians(self.set["glide_deg"])
        trk = math.atan2(v[0], v[1]) if np.linalg.norm(v[:2]) > 1.0 else math.radians(p["hdg_true"])
        def at_elev(elev, a0, a1, n):
            """Screen polyline of the directions `elev` rad above the horizon, azimuth track+a0..a1 deg."""
            seg = []
            for daz in np.linspace(a0, a1, n):
                az = trk + math.radians(daz)
                q = pix(np.array([math.sin(az) * math.cos(elev), math.cos(az) * math.cos(elev), math.sin(elev)]))
                if q:
                    seg.append(q)
            return seg

        def glide(a0, a1, n):
            return at_elev(-g, a0, a1, n)

        span = min(self.set["line_span_deg"], 10.0 * self.set.get("hud_scale", 1.0))  # inside the HUD, clear of the tapes
        rwy_poly = None
        if runway is not None:
            # runway is a real place: camera position = aircraft datum + ChasePlane eye offset (body frame)
            R_ac = C.aircraft_rotation(p)
            eye = np.array([p.get("cp_eye_x", 0.0), p.get("cp_eye_y", 0.0), p.get("cp_eye_z", 0.0)])
            cam_pos = np.array([0.0, 0.0, p["alt_ft"] * FT]) + R_ac @ eye
            pts = RW.world_points_cam(runway.corners(), p["lat"], p["lon"], cam_pos, R)
            poly3 = RW.clip_near(pts)
            if len(poly3) >= 3:
                rwy_poly = [(W / 2 + f * q[0] / q[2], H / 2 - f * q[1] / q[2]) for q in poly3]
        if rwy_poly:
            # continuous line, cut only where it runs across the runway outline
            full = glide(-span, span, 73)
            line = RW.clip_segments_outside(full, rwy_poly) if len(full) > 1 else []
        else:
            # no runway known: fixed gap either side of the track
            gap = min(self.set["line_gap_deg"], span - 1)
            line = [seg for seg in (glide(-span, -gap, 13), glide(gap, span, 13)) if len(seg) > 1]

        return bird, line, f, rwy_poly, {"R": R, "p": p}


def usable(s):
    """Why the overlay should stay hidden, or None."""
    if s is None:
        return "等 SimConnect 数据"
    if s["cp_zoom"] <= 0.05:
        return "没读到 ChasePlane 变量"
    if s["cp_mode"] >= 0.5:
        return "外部视角"
    if abs(s["cp_yaw"]) > 45 or abs(s["cp_pitch"]) > 40:
        return "没在看前方"
    if s["gs_kt"] < 40 and s["on_ground"] >= 0.5:
        return "地面低速"
    return None


# ---------------------------------------------------------------- drawing (Qt)
def flare_target(ra, approach_fpa):
    """Expected flight path angle (relative to the runway) for a radio height: the approach FPA down to 40 ft,
    then linearly to −0.5° at 10 ft, −0.5° below."""
    if ra >= 40.0:
        return approach_fpa
    if ra <= 10.0:
        return -0.5
    return -0.5 + (approach_fpa + 0.5) * (ra - 10.0) / 30.0


def flare_cue(model, s, runway, settings, now=None):
    """Flare helper below 50 ft RA (airborne): 1 = on the profile (within the tolerance), 2 = above it (shallower
    than expected), 0 = below it (still too steep: keep pulling); with a blink phase. None = not active."""
    if not settings.get("show_flare", True) or s.get("on_ground", 0):
        return None
    ra = s.get("radio_alt_ft")
    fpa = model.fpa_deg()
    if ra is None or fpa is None or not (0.0 < ra <= 50.0):
        return None
    slope = runway.slope if runway is not None else 0.0  # + = uphill in the landing direction
    rel = fpa - slope
    tgt = flare_target(ra, -abs(settings.get("glide_deg", 3.0)))
    tol = settings.get("flare_tol_deg", 0.3)
    n = 1 if abs(rel - tgt) <= tol else (2 if rel > tgt else 0)
    now = time.perf_counter() if now is None else now
    return {"n": n, "on": (now * 4.0) % 1.0 < 0.6, "rel": rel, "target": tgt, "ra": ra}


def hud_scene(model, s, W, H, ox, oy, settings, runway=None, flash=None, fd=None):
    """Full HUD for one frame as a hud.build() scene: conformal symbols + airframe-fixed tapes (see hud.py)."""
    bird, line, f, rwy_poly, ex = model.geometry(s, W, H, runway)
    p = ex["p"]
    fr = HUD.Frame(p, ex["R"], f, W, H, ox, oy, settings.get("hud_scale", 1.0))
    conf = HUD.conformal(fr, settings, p["hdg_true"], p.get("hdg_mag", p["hdg_true"]))
    sh = lambda pts: [(x - ox, y - oy) for x, y in pts]  # noqa: E731
    rp = sh(rwy_poly) if rwy_poly else None
    if rp:  # nothing conformal is drawn across the runway
        if conf.get("horizon"):
            conf["horizon_pieces"] = RW.clip_segments_outside(conf["horizon"], rp)
        conf["rungs"] = [(e, [q for seg in segs for q in RW.clip_segments_outside(seg, rp)], lab)
                         for e, segs, lab in conf["rungs"]]
        conf["minor"] = [q for seg in conf.get("minor", []) for q in RW.clip_segments_outside(seg, rp)]
    kw = dict(bird=(bird[0] - ox, bird[1] - oy) if bird else None,
              glide=[sh(seg) for seg in line] if line else None, glide_deg=settings["glide_deg"],
              rwy_poly=rp, rwy_label=runway.name if (runway and rp) else None, flash=flash, fd=fd,
              fpa=model.fpa_deg(), vs_fpm=model.vs_fpm(), flare=flare_cue(model, s, runway, settings))
    return fr, p, conf, kw


def paint_hud(qp, model, s, W, H, ox, oy, settings, runway=None, flash=None, fd=None):
    """QPainter version (offline snapshots / fallback window)."""
    fr, p, conf, kw = hud_scene(model, s, W, H, ox, oy, settings, runway, flash, fd)
    HUD.paint(qp, fr, p, conf, model.filt, settings, **kw)


def hud_commands(model, s, W, H, ox, oy, settings, runway=None, flash=None, fd=None):
    """GPU renderer input (hudgfx.dll) for one frame."""
    fr, p, conf, kw = hud_scene(model, s, W, H, ox, oy, settings, runway, flash, fd)
    return HUD.commands(HUD.build(fr, p, conf, model.filt, settings, **kw), settings.get("opacity", 1.0))


# ---------------------------------------------------------------- win32 helpers
user32 = ctypes.WinDLL("user32", use_last_error=True)
MOD_ALT, MOD_CONTROL, MOD_NOREPEAT = 0x1, 0x2, 0x4000
HOTKEYS = {  # name: (modifiers, vk, label)
    "up": (MOD_CONTROL | MOD_ALT, 0x21, "Ctrl+Alt+PgUp"),
    "down": (MOD_CONTROL | MOD_ALT, 0x22, "Ctrl+Alt+PgDn"),
    "left": (MOD_CONTROL | MOD_ALT, 0x23, "Ctrl+Alt+End"),
    "right": (MOD_CONTROL | MOD_ALT, 0x24, "Ctrl+Alt+Home"),
    "f": (MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 0x2D, "Ctrl+Alt+Insert"),
    "q": (MOD_CONTROL | MOD_ALT | 0x4 | MOD_NOREPEAT, 0x23, "Ctrl+Alt+Shift+End"),
    "fd": (MOD_CONTROL | 0x4 | MOD_NOREPEAT, 0x76, "Ctrl+Shift+F7"),
}
WM_HOTKEY = 0x0312


def find_msfs():
    hits = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        if buf.value.startswith("Microsoft Flight Simulator") and user32.IsWindowVisible(h):
            hits.append(h)
        return True
    user32.EnumWindows(cb, 0)
    return hits[0] if hits else None


def no_frame(hwnd):
    """Windows 11 draws a 1 px rounded accent border (and a shadow) around frameless top-level windows: turn off."""
    try:
        dwm = ctypes.WinDLL("dwmapi")
        for attr, val in ((2, 1),            # DWMWA_NCRENDERING_POLICY = DWMNCRP_DISABLED
                          (33, 1),           # DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_DONOTROUND
                          (34, 0xFFFFFFFE)):  # DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE
            v = ctypes.c_uint(val)
            dwm.DwmSetWindowAttribute(wintypes.HWND(hwnd), attr, ctypes.byref(v), ctypes.sizeof(v))
    except OSError:
        pass


def client_rect(h):
    """Client area in physical screen pixels: (x, y, w, h)."""
    r = wintypes.RECT()
    if not user32.GetClientRect(h, ctypes.byref(r)):
        return None
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(h, ctypes.byref(pt))
    return pt.x, pt.y, r.right, r.bottom


def hotkey_thread(q, stop):
    names = list(HOTKEYS)
    for i, n in enumerate(names, start=1):
        mod, vk, label = HOTKEYS[n]
        if not user32.RegisterHotKey(None, i, mod, vk):
            q.put(("warn", f"{label} 注册失败（错误 {ctypes.get_last_error()}，被占用？）"))
    msg = wintypes.MSG()
    while not stop.is_set():
        if user32.PeekMessageW(ctypes.byref(msg), None, WM_HOTKEY, WM_HOTKEY, 1):
            if msg.message == WM_HOTKEY and 1 <= msg.wParam <= len(names):
                q.put(("key", names[msg.wParam - 1]))
        else:
            time.sleep(0.01)
    for i in range(1, len(names) + 1):
        user32.UnregisterHotKey(None, i)


# ---------------------------------------------------------------- tray
def autostart_command():
    pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    return f'"{pyw}" "{os.path.join(HERE, "fpv_tray.pyw")}"'


def autostart_enabled():
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            return winreg.QueryValueEx(k, RUN_NAME)[0] == autostart_command()
    except OSError:
        return False


def set_autostart(on):
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if on:
            winreg.SetValueEx(k, RUN_NAME, 0, winreg.REG_SZ, autostart_command())
        else:
            try:
                winreg.DeleteValue(k, RUN_NAME)
            except FileNotFoundError:
                pass


def tray_icon(active):
    """The bird itself: green while drawing over the sim, grey otherwise."""
    from PyQt6.QtCore import QPointF, Qt
    from PyQt6.QtGui import QColor, QIcon, QPainter, QPainterPath, QPen, QPixmap
    px = QPixmap(64, 64)
    px.fill(Qt.GlobalColor.transparent)
    p = QPainter(px)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    path = QPainterPath()
    path.addEllipse(QPointF(32, 36), 11, 11)
    path.moveTo(21, 36)
    path.lineTo(3, 36)
    path.moveTo(43, 36)
    path.lineTo(61, 36)
    path.moveTo(32, 25)
    path.lineTo(32, 11)
    for color, width in ((QColor(0, 0, 0, 170), 10), (QColor(70, 255, 110) if active else QColor(160, 160, 160), 5.5)):
        pen = QPen(color, width)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.drawPath(path)
    p.end()
    return QIcon(px)


class Tray:
    """Resident tray icon: right-click menu with the display switches, double-click toggles the overlay."""
    GLIDES = (2.5, 2.8, 3.0, 3.2, 3.5, 4.0, 4.5, 5.0, 5.5)
    LEADS = (0, 20, 40, 60, 80, 100, 150)

    def __init__(self, app, ov, settings, stop):
        from PyQt6.QtCore import QTimer
        from PyQt6.QtGui import QAction, QActionGroup
        from PyQt6.QtWidgets import QMenu, QSystemTrayIcon
        self.app, self.ov, self.set, self.stop = app, ov, settings, stop
        self.active = None
        self.tray = QSystemTrayIcon(tray_icon(False))
        m = self.menu = QMenu()
        self.acts = {}

        def toggle(key, label):
            a = QAction(label, m, checkable=True)
            a.setChecked(bool(settings[key]))
            a.toggled.connect(lambda on, k=key: self.set_value(k, on))
            m.addAction(a)
            self.acts[key] = a

        toggle("visible", "显示覆盖层\tCtrl+Alt+Insert")
        m.addSeparator()
        toggle("show_bird", "小鸟（飞行路径矢量）")
        toggle("show_glide", "下滑参考线")
        toggle("show_horizon", "地平线")
        toggle("show_runway", "跑道边框")
        hud_menu = m.addMenu("HUD 仪表")
        for key, label in (("show_att", "飞机姿态基准符号"), ("show_fd", "FD（GlassOut 或弹出的 PFD）	Ctrl+Shift+F7"), ("show_speed", "速度带（含马赫、速度趋势）"), ("show_alt", "高度带（含气压、无线电高度）"),
                           ("show_vs", "升降速度"), ("show_roll", "坡度刻度"), ("show_pitch_minor", "俯仰 2.5° 小刻度"), ("show_fpa_num", "小鸟右侧：航迹角数字"), ("show_vs_num", "小鸟左侧：升降速度数字"),
                           ("fd_ra_cutoff", "无线电高度 200 ft 以下自动关 FD"),
                           ("show_flare", "拉平提示（50 ft 以下，小鸟上方闪 + / ++）"), ("show_hdg", "航向刻度（地平线上）"),
                           ("show_gs", "地速 / 风")):
            a = QAction(label, hud_menu, checkable=True)
            a.setChecked(bool(settings.get(key, True)))
            a.toggled.connect(lambda on, k=key: self.set_value(k, on))
            hud_menu.addAction(a)
            self.acts[key] = a
        m.addSeparator()
        self.glide_menu = m.addMenu("下滑角")
        self.glide_group = QActionGroup(self.glide_menu)
        self.glide_acts = {}
        for g in self.GLIDES:
            a = QAction(f"{g:.1f}°", self.glide_menu, checkable=True)
            a.triggered.connect(lambda _c, v=g: self.set_value("glide_deg", v))
            self.glide_group.addAction(a)
            self.glide_menu.addAction(a)
            self.glide_acts[g] = a
        self.glide_menu.addSeparator()
        hint = self.glide_menu.addAction("微调：Ctrl+Alt+PgUp / PgDn（±0.1°）")
        hint.setEnabled(False)
        self.lead_menu = m.addMenu("提前量（补偿显示延迟）")
        self.lead_group = QActionGroup(self.lead_menu)
        self.lead_acts = {}
        for v in self.LEADS:
            a = QAction(f"{v} ms", self.lead_menu, checkable=True)
            a.triggered.connect(lambda _c, val=v: self.set_value("lead_ms", val))
            self.lead_group.addAction(a)
            self.lead_menu.addAction(a)
            self.lead_acts[v] = a
        self.lead_menu.addSeparator()
        hint = self.lead_menu.addAction("微调：Ctrl+Alt+Home / End（±10 ms）")
        hint.setEnabled(False)
        self.tol_menu, self.tol_acts = self.choice_menu(
            m, "拉平提示容差", "flare_tol_deg", [(v, f"±{v:.1f}°") for v in (0.2, 0.3, 0.5, 0.8)])
        fd_menu = m.addMenu("FD 圆圈平滑（TRK-FPA）")
        self.lag_menu, self.lag_acts = self.choice_menu(
            fd_menu, "画面延迟补偿", "fpd_lag_ms", [(v, f"{v} ms") for v in (0, 60, 90, 120, 150, 200, 250)])
        self.tau_menu, self.tau_acts = self.choice_menu(
            fd_menu, "平滑时间", "fpd_tau_ms", [(v, "不平滑" if v == 0 else f"{v} ms") for v in (0, 50, 100, 150, 200, 300)])
        fd_menu.addSeparator()
        hint = fd_menu.addAction("压坡度时圆圈先冲过头再回来 → 调延迟补偿；还抖 → 加大平滑时间")
        hint.setEnabled(False)
        self.ladder_menu, self.ladder_acts = self.choice_menu(
            m, "航迹倾角线", "ladder_step_deg", [(0.0, "关")] + [(v, f"每 {v:.0f}°") for v in (3.0, 5.0, 6.0, 10.0)])
        self.size_menu, self.size_acts = self.choice_menu(
            m, "HUD 大小", "hud_scale", [(v / 100, f"{v}%") for v in (60, 65, 70, 75, 80, 85, 90, 100, 115)])
        self.opac_menu, self.opac_acts = self.choice_menu(
            m, "透明度", "opacity", [(v / 100, f"{100 - v}% 透明" if v < 100 else "不透明") for v in (100, 85, 70, 55, 40, 25)])
        m.addSeparator()
        self.status_act = m.addAction("状态")
        self.status_act.setEnabled(False)
        self.rwy_act = m.addAction("跑道")
        self.rwy_act.setEnabled(False)
        m.addSeparator()
        self.ra_menu = m.addMenu("RA 校准")
        self.ra_menu.addAction("在地面校准（当前读数记为 0 ft）").triggered.connect(lambda: self.ov.calibrate_ra())
        self.ra_menu.addAction("清除校准").triggered.connect(lambda: self.ov.clear_ra())
        self.ra_note = self.ra_menu.addAction("")
        self.ra_note.setEnabled(False)
        m.addSeparator()
        m.addAction("打开日志").triggered.connect(lambda: os.startfile(LOG_PATH))
        self.auto_act = QAction("开机自启", m, checkable=True)
        self.auto_act.setChecked(autostart_enabled())
        self.auto_act.toggled.connect(self.toggle_autostart)
        m.addAction(self.auto_act)
        m.addAction("退出").triggered.connect(self.quit)
        self.tray.setContextMenu(m)
        self.tray.activated.connect(self.on_click)
        self.tray.show()
        self.timer = QTimer()
        self.timer.timeout.connect(self.refresh)
        self.timer.start(500)
        self.refresh()

    def choice_menu(self, parent, title, key, choices):
        from PyQt6.QtGui import QAction, QActionGroup
        sub = parent.addMenu(title)
        group = QActionGroup(sub)
        acts = {}
        for val, label in choices:
            a = QAction(label, sub, checkable=True)
            a.triggered.connect(lambda _c, v=val: self.set_value(key, v))
            group.addAction(a)
            sub.addAction(a)
            acts[val] = a
        sub._group = group  # keep the group alive with the menu
        return sub, acts

    def set_value(self, key, val):
        if self.set.get(key) == val:
            return
        self.set[key] = val
        save_settings(self.set)
        print(f"[tray] {key} = {val}")
        self.ov.redraw()
        self.refresh()

    def toggle_autostart(self, on):
        try:
            set_autostart(on)
            print(f"[tray] 开机自启 {'开' if on else '关'}")
        except OSError as e:
            print(f"[tray] 开机自启设置失败: {e}")

    def on_click(self, reason):
        from PyQt6.QtWidgets import QSystemTrayIcon
        if reason == QSystemTrayIcon.ActivationReason.DoubleClick:
            self.acts["visible"].toggle()

    def quit(self):
        self.stop.set()
        self.tray.hide()
        self.app.quit()

    def refresh(self):
        s = self.set
        for key, a in self.acts.items():  # hotkeys change settings too: keep the ticks in sync
            if a.isChecked() != bool(s.get(key, True)):
                a.blockSignals(True)
                a.setChecked(bool(s.get(key, True)))
                a.blockSignals(False)
        g = round(s["glide_deg"], 1)
        self.glide_menu.setTitle(f"下滑角：{g:.1f}°")
        for v, a in self.glide_acts.items():
            a.setChecked(abs(v - g) < 1e-6)
        if g not in self.glide_acts:
            ch = self.glide_group.checkedAction()
            if ch:
                self.glide_group.setExclusive(False)
                ch.setChecked(False)
                self.glide_group.setExclusive(True)
        self.lead_menu.setTitle(f"提前量（补偿显示延迟）：{s['lead_ms']} ms")
        for v, a in self.lead_acts.items():
            a.setChecked(v == s["lead_ms"])
        step = s.get("ladder_step_deg", 0) or 0
        self.ladder_menu.setTitle("航迹倾角线：" + (f"每 {step:.0f}°" if step else "关"))
        for v, a in self.ladder_acts.items():
            a.setChecked(abs(v - step) < 1e-6)
        off = s.get("ra_offset_ft", 0.0)
        self.ra_menu.setTitle(f"RA 校准：−{off:.1f} ft" if off else "RA 校准：未校准")
        self.ra_note.setText(s.get("ra_offset_note") or "停在地面上点上面一项")
        self.tol_menu.setTitle(f"拉平提示容差：±{s.get('flare_tol_deg', 0.3):.1f}°")
        for v, a in self.tol_acts.items():
            a.setChecked(abs(v - s.get("flare_tol_deg", 0.3)) < 1e-6)
        lag, tau = s.get("fpd_lag_ms", 120), s.get("fpd_tau_ms", 100)
        self.lag_menu.setTitle(f"画面延迟补偿：{lag} ms")
        for v, a in self.lag_acts.items():
            a.setChecked(v == lag)
        self.tau_menu.setTitle(f"平滑时间：{tau} ms" if tau else "平滑时间：不平滑")
        for v, a in self.tau_acts.items():
            a.setChecked(v == tau)
        k = s.get("hud_scale", 1.0)
        self.size_menu.setTitle(f"HUD 大小：{round(k * 100)}%（约 {35 * k:.0f}°×{28 * k:.0f}°）")
        for v, a in self.size_acts.items():
            a.setChecked(abs(v - k) < 1e-6)
        op = s.get("opacity", 1.0)
        self.opac_menu.setTitle(f"透明度：{100 - round(op * 100)}%")
        for v, a in self.opac_acts.items():
            a.setChecked(abs(v - op) < 1e-6)
        why = self.ov.why
        drawing = bool(s["visible"] and not why)
        status = "正在显示" if drawing else ("已关闭" if not s["visible"] else f"隐藏：{why}")
        rw = self.ov.runway
        rtxt = f"跑道：{rw.icao} {rw.name}（{rw.length:.0f}×{rw.width:.0f} m）" if rw else "跑道：未选中"
        pfd = getattr(self, "pfd", None)
        if pfd is not None:
            pfd.enabled = bool(s.get("show_fd", True))
            rtxt += " · FD：" + (pfd.status if pfd.enabled else "关")
        self.status_act.setText(f"状态：{status}")
        self.rwy_act.setText(rtxt)
        self.tray.setToolTip(f"FPV 覆盖层\n{status}\n{rtxt}\n下滑角 {g:.1f}°")
        if drawing != self.active:
            self.active = drawing
            self.tray.setIcon(tray_icon(drawing))


# ---------------------------------------------------------------- live app
def run_live(args):
    os.environ.setdefault("QT_ENABLE_HIGHDPI_SCALING", "1")
    from PyQt6.QtCore import Qt, QTimer
    from PyQt6.QtGui import QColor, QPainter
    from PyQt6.QtWidgets import QApplication, QWidget
    from simconnect_min import PERIOD_SIM_FRAME, SimConnect, SimConnectError

    class SC(SimConnect):
        def poll(self):
            """simconnect_min.poll plus ('raw', recv_id, bytes) for the facility messages."""
            ptr, size = ctypes.c_void_p(), wintypes.DWORD()
            while self.dll.SimConnect_GetNextDispatch(self.h, ctypes.byref(ptr), ctypes.byref(size)) == 0:
                raw = ctypes.string_at(ptr.value, size.value)
                _, _, rid = struct.unpack_from("<III", raw, 0)
                if rid == 8:
                    request_id, _o, _d, _f, _n, _of, count = struct.unpack_from("<7I", raw, 12)
                    yield ("data", request_id, count, raw[40:])
                elif rid == 1:
                    yield ("exception",) + struct.unpack_from("<III", raw, 12)
                elif rid == 3:
                    yield ("quit",)
                elif rid in (RW.RECV_AIRPORT_LIST, RW.RECV_FACILITY_DATA, RW.RECV_FACILITY_DATA_END):
                    yield ("raw", rid, raw)

    settings = load_settings()
    model = Model(settings)
    keys = queue.Queue()
    stop = threading.Event()
    threading.Thread(target=hotkey_thread, args=(keys, stop), daemon=True).start()

    app = QApplication(sys.argv)
    try:  # FD reading (GlassOut / PFD pixels) is phase 3 and not part of this repository
        import pfd_reader
        pfd = pfd_reader.PfdReader()
    except ImportError:
        class _NoFd:
            enabled, source, status = False, None, "phase 3 (not included)"

            def latest(self):
                return None
        pfd = _NoFd()
    fpd_track = FpdTracker(settings)

    ra_cut = {"low": False}

    def fd_now(state=None):
        if not settings.get("show_fd", True):
            return None
        if settings.get("fd_ra_cutoff", True) and state:
            # below 200 ft RA (in the air) the FD goes away; back above 250 ft (go-around), hysteresis against flicker
            ra = state.get("radio_alt_ft")
            if ra is not None:
                if ra < 200 and not state.get("on_ground", 0):
                    ra_cut["low"] = True
                elif ra > 250:
                    ra_cut["low"] = False
            if ra_cut["low"]:
                return None
        return fpd_track.apply(pfd.latest(), time.perf_counter())

    class Overlay(QWidget):
        def __init__(self):
            super().__init__(None, Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint
                             | Qt.WindowType.Tool | Qt.WindowType.WindowTransparentForInput
                             | Qt.WindowType.WindowDoesNotAcceptFocus | Qt.WindowType.NoDropShadowWindowHint)
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
            self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
            self.sc = None
            self.fac = None
            self.runway = None
            self.t_airports = 0.0
            self.state = None
            self.t_state = 0.0
            self.hwnd_sim = None
            self.client = None
            self.box = None  # (x, y, w, h) of this window in physical screen px
            self.flash = None
            self.flash_until = 0.0
            self.why = "启动中"
            self.paint_ms = 0.0
            self.build_ms = 0.0
            self.gfx = None
            self.gfx_rc = 0
            self.use_gfx = os.environ.get("FPV_QT_PAINT") != "1"
            self.n_frames = 0
            self.n_msgs = 0
            from collections import deque
            self.ra_ground = deque(maxlen=60)  # raw RA samples while on the ground (for the calibration)
            self.tick_gap = 0.0
            self.t_tick = time.perf_counter()
            self.data_age = []  # ms from SimConnect message to the end of its paint
            self.t_stat = time.perf_counter()
            self.last_print = 0.0
            self.timer = QTimer(self)
            self.timer.setTimerType(Qt.TimerType.PreciseTimer)
            self.timer.timeout.connect(self.tick)
            self.timer.start(4)
            self.slow = QTimer(self)
            self.slow.timeout.connect(self.housekeeping)
            self.slow.start(500)
            self.housekeeping()

        # -- connections and window placement
        def connect_sim(self):
            if args.demo:
                self.why = None
                return
            try:
                self.sc = SC("fpv_overlay")
            except (SimConnectError, OSError) as e:
                self.sc = None
                self.why = "模拟器没开"
                return
            for i, (_k, name, unit) in enumerate(VARS):
                self.sc.add_var(1, name, unit)
            self.sc.request(1, 1, PERIOD_SIM_FRAME)
            self.fac = RW.FacilityClient(self.sc)
            self.t_airports = 0.0
            print("[sim] SimConnect 已连接")

        def housekeeping(self):
            if self.sc is None:
                self.connect_sim()
            if args.demo:
                g = app.primaryScreen().geometry()
                dpr = app.primaryScreen().devicePixelRatio()
                h, cr = None, (int(g.x() * dpr), int(g.y() * dpr), int(g.width() * dpr), int(g.height() * dpr))
            else:
                h = find_msfs()
                cr = client_rect(h) if h else None
            self.hwnd_sim = h
            self.update_runway()
            self.redraw()  # re-evaluate hide reasons even when no data arrives (pause, focus change)
            if cr and cr[2] > 100 and cr != self.client:
                self.client = cr
                x, y, w, hh = cr
                bw, bh = int(w * settings["box_w"]), int(hh * settings["box_h"])
                self.box = (x + (w - bw) // 2, y + (hh - bh) // 2, bw, bh)
                self.place()
                print(f"[win] MSFS 客户区 {w}x{hh} @({x},{y})，覆盖层 {bw}x{bh}")
            now = time.perf_counter()
            if now - self.last_print > (1 if args.demo else 5) and self.state:
                s = self.state
                fps = self.n_frames / max(now - self.t_stat, 1e-3)
                mps = self.n_msgs / max(now - self.t_stat, 1e-3)
                ages = sorted(self.data_age)
                lat = f"{ages[len(ages) // 2]:.0f}/{ages[int(len(ages) * 0.95)]:.0f}" if ages else "-"
                print(f"[perf] {'GPU' if self.gfx else 'Qt'}  算几何 {self.build_ms:4.1f} ms  数据 {mps:4.1f}/s  tick 最大间隔 {self.tick_gap * 1000:4.0f} ms  取数→画完 中位/95% {lat} ms  "
                      f"FD 来源 {pfd.source or '无'}")
                self.n_msgs, self.tick_gap, self.data_age = 0, 0.0, []
                vs_world = s["vy_fps"] * 60
                print(f"[live] {fps:4.0f} Hz  绘制 {self.paint_ms:4.1f} ms  gs {s['gs_kt']:5.1f} kt  "
                      f"VS 表 {s['vs_fpm']:+6.0f} / 世界 {vs_world:+6.0f} fpm  agl {s['agl_ft']:6.0f}  "
                      f"cp y/p {s['cp_yaw']:+.1f}/{s['cp_pitch']:+.1f} z {s['cp_zoom']:.3f}  "
                      f"跑道 {self.runway.icao + ' ' + self.runway.name if self.runway else '—'}  "
                      f"{'隐藏: ' + self.why if self.why else '显示'}")
                self.n_frames, self.t_stat, self.last_print = 0, now, now

        def update_runway(self):
            if not (self.fac and self.state):  # selected even with the outline off: the glide line still clears it
                return
            s, now = self.state, time.perf_counter()
            if now - self.t_airports > 20:  # airports in the reality bubble; runways fetched once per airport
                self.t_airports = now
                self.fac.request_airports()
            near = self.fac.nearby(s["lat"], s["lon"])
            for icao in near:
                self.fac.request_runways(icao)
            rw = RW.pick(self.fac.all_ends(near), s["lat"], s["lon"], s["trk_true"], self.runway)
            if rw is not self.runway:
                print(f"[rwy] 选中: {rw}" if rw else "[rwy] 没有对正的跑道")
                self.runway = rw

        def place(self):
            if not self.box:
                return
            x, y, w, h = self.box
            if self.use_gfx and self.gfx is None:
                try:
                    import hudgfx
                    self.gfx = hudgfx.HudGfx(x, y, w, h)
                    print("[gfx] GPU 绘制（hudgfx.dll，Direct2D + DirectComposition）")
                except Exception as e:  # missing DLL / no D3D11: fall back to the QPainter window
                    self.use_gfx = False
                    print(f"[gfx] 原生绘制不可用，退回 Qt 绘制：{e}")
            if self.gfx is not None:
                self.gfx.move(x, y, w, h)
                self.gfx.show(True)
                return
            dpr = self.screen().devicePixelRatio() if self.screen() else 1.0
            self.setGeometry(int(x / dpr), int(y / dpr), int(math.ceil(w / dpr)), int(math.ceil(h / dpr)))
            if not self.isVisible():
                self.show()
            hwnd = int(self.winId())
            ex = user32.GetWindowLongW(hwnd, -20)
            user32.SetWindowLongW(hwnd, -20, ex | 0x20 | 0x80000 | 0x8000000 | 0x80)  # transparent|layered|noactivate|toolwindow
            no_frame(hwnd)
            # physical-pixel placement (Qt geometry is logical)
            user32.SetWindowPos(hwnd, -1, x, y, w, h, 0x10)  # HWND_TOPMOST, SWP_NOACTIVATE

        # -- per tick
        def tick(self):
            tnow = time.perf_counter()
            self.tick_gap = max(self.tick_gap, tnow - self.t_tick)
            self.t_tick = tnow
            while not keys.empty():
                kind, val = keys.get()
                if kind == "warn":
                    print("[hotkey]", val)
                    continue
                if val == "q":
                    stop.set()
                    app.quit()
                    return
                if val == "f":
                    settings["visible"] = not settings["visible"]
                    self.say("覆盖层 " + ("开" if settings["visible"] else "关"))
                elif val == "fd":
                    settings["show_fd"] = not settings.get("show_fd", True)
                    self.say("FD " + ("开" if settings["show_fd"] else "关"))
                elif val in ("up", "down"):
                    settings["glide_deg"] = round(min(10.0, max(0.0, settings["glide_deg"]
                                                                 + (0.1 if val == "up" else -0.1))), 1)
                    self.say(f"下滑参考线 −{settings['glide_deg']:.1f}°")
                elif val in ("left", "right"):
                    settings["lead_ms"] = int(min(300, max(-100, settings["lead_ms"] + (10 if val == "right" else -10))))
                    self.say(f"提前量 {settings['lead_ms']} ms")
                save_settings(settings)
                self.redraw()
            if args.demo:
                t = time.perf_counter()
                self.state = {"hdg_true": 250 + 2 * math.sin(t), "pitch_deg": -2.0 + 0.5 * math.sin(1.3 * t),
                              "bank_deg": 3 * math.sin(0.7 * t), "trk_true": 250.0, "gs_kt": 140.0,
                              "vy_fps": -12.0, "vs_fpm": -720.0, "alt_ft": 800.0, "agl_ft": 700.0,
                              "on_ground": 0.0, "cp_mode": 0.0, "cp_pitch": -6.5, "cp_yaw": 0.0, "cp_roll": 0.0,
                              "cp_zoom": 0.497}
                if t - self.t_state > 1 / 60:
                    self.t_state = t
                    model.update(self.state, t)
                    self.n_frames += 1
                    self.redraw()
                return
            if self.sc is None:
                return
            got = False
            try:
                for m in self.sc.poll():
                    if m[0] == "data" and m[1] == 1:
                        vals = struct.unpack_from(f"<{len(VARS)}d", m[3])
                        self.state = {k: v for (k, _n, _u), v in zip(VARS, vals)}
                        # the sim's radio altimeter reads several ft on the ground (gear not accounted for):
                        # subtract the offset measured on the ground (tray: RA 地面校准, kept in settings.json)
                        raw = self.state["radio_alt_ft"]
                        self.state["ra_raw_ft"] = raw
                        self.state["radio_alt_ft"] = raw - settings.get("ra_offset_ft", 0.0)
                        if self.state.get("on_ground", 0) and self.state.get("gs_kt", 99) < 30:
                            self.ra_ground.append(raw)
                        else:
                            self.ra_ground.clear()
                        self.t_state = time.perf_counter()
                        model.update(self.state, self.t_state)
                        fpd_track.add(self.t_state, model, self.state)
                        self.n_msgs += 1
                        got = True
                    elif m[0] == "quit":
                        print("[sim] 模拟器退出")
                        self.sc.close()
                        self.sc, self.state = None, None
                        self.redraw()
                        return
                    elif m[0] == "raw":
                        if self.fac:
                            self.fac.handle(m[1], m[2])
                    elif m[0] == "exception":
                        print("[sim] exception", m[1:])
            except OSError:
                self.sc, self.state = None, None
            if got:
                self.n_frames += 1
                self.redraw()
            elif self.flash and time.perf_counter() > self.flash_until:
                self.flash = None
                self.redraw()

        def calibrate_ra(self):
            """Tray: take the current on-ground radio altitude as the zero point and keep it in settings.json."""
            if not self.state or not self.state.get("on_ground", 0) or len(self.ra_ground) < 10:
                self.say("RA 校准：要停在地面上（地速 < 30 kt）再点")
                return
            off = float(np.median(list(self.ra_ground)))
            settings["ra_offset_ft"] = round(off, 1)
            settings["ra_offset_note"] = time.strftime("%Y-%m-%d %H:%M") + f" 地面读数 {off:.1f} ft"
            save_settings(settings)
            self.say(f"RA 已校准：地面读数 {off:.1f} ft 记为 0")

        def clear_ra(self):
            settings["ra_offset_ft"], settings["ra_offset_note"] = 0.0, ""
            save_settings(settings)
            self.say("RA 校准已清除")

        def say(self, text):
            self.flash, self.flash_until = text, time.perf_counter() + 2.5
            print("[set]", text)

        def compute_why(self):
            fg = user32.GetForegroundWindow()
            self.why = usable(self.state) if self.state else ("模拟器没开" if self.sc is None else "等数据")
            if not self.why and fg != self.hwnd_sim and not args.demo:
                self.why = "MSFS 不在前台"
            if not self.why and time.perf_counter() - self.t_state > 0.5:
                self.why = "数据停了（暂停？）"
            return bool(settings["visible"] and not self.why and self.client and self.box)

        def redraw(self):
            if self.gfx is None:
                self.update()
                return
            t0 = time.perf_counter()
            if self.compute_why():
                ox, oy = self.box[0] - self.client[0], self.box[1] - self.client[1]
                cmds, txt = hud_commands(model, self.state, self.client[2], self.client[3], ox, oy, settings,
                                         self.runway, self.flash,
                                         fd_now(self.state))
            elif self.flash:
                F = HUD.Batch()
                F.text(14, 26, self.flash, 15, "start")
                cmds, txt = HUD.commands({"main": None, "flash": F}, settings.get("opacity", 1.0))
            else:
                cmds, txt = [], ""
            t1 = time.perf_counter()
            rc, err = self.gfx.frame(cmds, txt)
            if rc and rc != self.gfx_rc:
                print(f"[gfx] 绘制错误 {rc}: {err}")
            self.gfx_rc = rc
            t2 = time.perf_counter()
            self.build_ms = 0.9 * self.build_ms + 0.1 * (t1 - t0) * 1000
            self.paint_ms = 0.9 * self.paint_ms + 0.1 * (t2 - t0) * 1000
            if self.state and not self.why:
                self.data_age.append((t2 - self.t_state) * 1000)

        def paintEvent(self, _ev):
            t0 = time.perf_counter()
            p = QPainter(self)
            p.setCompositionMode(QPainter.CompositionMode.CompositionMode_Clear)
            p.fillRect(self.rect(), Qt.GlobalColor.transparent)
            p.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
            dpr = self.devicePixelRatioF()
            p.scale(1 / dpr, 1 / dpr)  # draw in physical pixels
            if self.compute_why():
                ox, oy = self.box[0] - self.client[0], self.box[1] - self.client[1]
                paint_hud(p, model, self.state, self.client[2], self.client[3], ox, oy, settings,
                          self.runway, self.flash, fd_now(self.state))
            elif self.flash:
                F = HUD.Batch()
                F.text(14, 26, self.flash, 15, "start")
                F.paint(p, QColor(0, 255, 0), QColor(0, 0, 0, 190))
            p.end()
            t1 = time.perf_counter()
            self.paint_ms = 0.9 * self.paint_ms + 0.1 * (t1 - t0) * 1000
            if self.state and not self.why:
                self.data_age.append((t1 - self.t_state) * 1000)

    import signal
    signal.signal(signal.SIGINT, lambda *_: app.quit())  # Ctrl+C in the console (timer ticks let Python see it)
    ov = Overlay()
    app.setQuitOnLastWindowClosed(False)
    tray = Tray(app, ov, settings, stop)  # noqa: F841
    tray.pfd = pfd
    if args.demo:
        QTimer.singleShot(int(args.demo * 1000), app.quit)
    print("FPV 覆盖层运行中（托盘图标右键是菜单）。Ctrl+Alt+PgUp/PgDn 下滑角，Ctrl+Alt+Home/End 提前量，"
          "Ctrl+Alt+Insert 开关，Ctrl+Shift+F7 FD 开关，Ctrl+Alt+Shift+End 退出")
    code = app.exec()
    stop.set()
    sys.exit(code)


# ---------------------------------------------------------------- offline snapshot
def run_snapshot(args):
    import subprocess
    sys.path.insert(0, os.path.join(HERE, "tools"))
    from foe_check import interp, load_track
    from obs_timeline import recordings_from_obs
    from PyQt6.QtGui import QGuiApplication, QImage, QPainter

    settings = load_settings()
    model = Model(settings)
    sdir = os.path.abspath(args.snapshot[0])
    vs = float(args.snapshot[1])
    rows = load_track(sdir)
    ts = [r["t"] for r in rows]
    with open(os.path.join(sdir, "obs.jsonl"), encoding="utf-8") as f:
        rec = recordings_from_obs([json.loads(x) for x in f if x.strip()])[0]
    t = (vs - rec["b"]) / rec["a"] + args.lag
    # feed the model a second of history so smoothing/rates settle; vertical from position (as VELOCITY WORLD Y would)
    for k in range(30, -1, -1):
        tk = t - k / 30
        s = interp(rows, ts, tk)
        a, b = interp(rows, ts, tk - 0.25), interp(rows, ts, tk + 0.25)
        s["vy_fps"] = (b["alt_ft"] - a["alt_ft"]) / 0.5
        s.setdefault("on_ground", 0.0)
        model.update(s, tk)
    raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{vs}", "-i", rec["path"], "-frames:v", "1", "-f",
                          "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
    probe = json.loads(subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                       "stream=width,height", "-of", "json", rec["path"]],
                                      capture_output=True, text=True).stdout)["streams"][0]
    W, H = probe["width"], probe["height"]
    app = QGuiApplication(sys.argv)  # must stay referenced while painting (fonts), or the process dies silently
    img = QImage(raw, W, H, W * 3, QImage.Format.Format_RGB888).copy()
    rw = None
    if args.runway:
        v = args.runway.split(",")
        rw = RW.Runway("TEST", float(v[0]), float(v[1]), float(v[2]), float(v[3]), float(v[4]), float(v[5]), v[6])
    # recordings lack some HUD vars: approximate them so the tapes have something to show
    s.setdefault("radio_alt_ft", s.get("agl_ft", 0.0))
    s.setdefault("baro_mb", 1013.0)
    s.setdefault("mach", s.get("ias_kt", 0.0) / 661.5)
    p = QPainter(img)
    t0 = time.perf_counter()
    paint_hud(p, model, s, W, H, 0, 0, settings, rw, f"snapshot v={vs:.2f}s  agl {s['agl_ft']:.0f} ft")
    ms = (time.perf_counter() - t0) * 1000
    p.end()
    out = args.out or os.path.join(HERE, "foe_out", f"snapshot_{vs:.0f}.png")
    img.save(out)
    print("->", out, f"(HUD 绘制 {ms:.1f} ms)")
    del app


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--snapshot", nargs=2, metavar=("SESSION", "VIDEO_S"))
    ap.add_argument("--lag", type=float, default=-0.34)
    ap.add_argument("--out")
    ap.add_argument("--runway", help="snapshot only: centre_lat,centre_lon,alt_m,landing_hdg_true,length_m,width_m,name")
    ap.add_argument("--demo", type=float, default=0, help="seconds of synthetic data on the primary screen (no sim)")
    args = ap.parse_args()
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    if args.snapshot:
        run_snapshot(args)
        return
    # live: log file always; console too when started with python.exe (pythonw has no stdout)
    log = _Log(sys.__stdout__ if sys.__stdout__ is not None else None)
    sys.stdout = sys.stderr = log
    if not args.demo:
        ctypes.windll.kernel32.CreateMutexW(None, False, "Local\\fpv_overlay_single_instance")
        if ctypes.windll.kernel32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS
            print("已经有一个 FPV 覆盖层在运行，退出")
            return
    run_live(args)


if __name__ == "__main__":
    main()
