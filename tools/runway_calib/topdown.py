"""Runway position from the picture: slew the aircraft above the runway, nose down, and measure the runway in the
screenshot. Ground truth for "where is the runway the sim draws", to compare the MSFS facility data with OurAirports.

Per cut (three places along each runway):
  1. Slew to `--height` m above the facility centreline, heading = runway heading, nose down `--pitch`.
  2. Wait for the scenery to stream in: ground altitude steady, then the picture itself steady (textures and
     photogrammetry arrive after the elevation).
  3. Screenshot the MSFS client; a second one 6 m to the side gives the cockpit mask (pixels that do not change).
  4. Rectify: every ground point (along, cross) of a strip around the facility centreline is projected into the
     picture with Birdie's camera model (aircraft attitude x ChasePlane head angles, eye offset, measured zoom ->
     vertical FOV) and sampled -> a top-down image in metres.
  5. Across-runway brightness profile; the edge stripes / edge lights are the two symmetric peaks -> centreline offset
     and width. Each half of the strip separately gives the heading difference.
Outputs per cut: ortho PNG + profile PNG (to check by eye) and the numbers in results.json / topdown.log.

Needs: ChasePlane cockpit view, no ESC menu, Birdie closed (no HUD in the picture), MSFS not minimised (it may be
covered: the MSFS window itself is captured with Windows Graphics Capture), daylight is best (night works with edge
lights). MOVES THE USER AIRCRAFT; puts it back and pauses.

    pip install --user windows-capture
    python tools/runway_calib/topdown.py ZSNB --out <folder> [--height 120] [--pitch 80]   (no -I: user site)
"""
import argparse
import ctypes
import json
import math
import os
import struct
import sys
import time
from ctypes import wintypes

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "zoom_calib"))
import collect as C  # noqa: E402

ctypes.windll.shcore.SetProcessDpiAwareness(2)
from PIL import Image, ImageDraw, ImageGrab  # noqa: E402

user32 = ctypes.windll.user32
D_CAM = 30
CAM_VARS = [("PLANE LATITUDE", "degrees"), ("PLANE LONGITUDE", "degrees"), ("PLANE ALTITUDE", "meters"),
            ("PLANE HEADING DEGREES TRUE", "degrees"), ("PLANE PITCH DEGREES", "degrees"),
            ("PLANE BANK DEGREES", "degrees"), ("GROUND ALTITUDE", "meters"), ("L:p42_cp_mode", "number"),
            ("L:p42_cp_r_yaw", "number"), ("L:p42_cp_r_pitch", "number"), ("L:p42_cp_r_roll", "number"),
            ("L:p42_cp_r_zoom", "number"), ("L:p42_cp_a_x", "number"), ("L:p42_cp_a_y", "number"),
            ("L:p42_cp_a_z", "number"), ("L:p42_cp_r_x", "number")]

# Birdie core/geo.h (measured 11/15)
ZOOM_REF, FOCAL_REF, VFOV_PER_ZOOM = 0.49725, 1298.0, -113.319


def focal_px(zoom, height):
    v_ref = 2 * math.degrees(math.atan(720.0 / FOCAL_REF))
    v = min(max(v_ref + VFOV_PER_ZOOM * (zoom - ZOOM_REF), 1.0), 170.0)
    return height / 2 / math.tan(math.radians(v / 2))


def aircraft_to_enu(hdg, pitch_up, bank_right):
    cy, sy = math.cos(math.radians(hdg)), math.sin(math.radians(hdg))
    cp, sp = math.cos(math.radians(pitch_up)), math.sin(math.radians(pitch_up))
    cr, sr = math.cos(math.radians(bank_right)), math.sin(math.radians(bank_right))
    fwd = np.array([sy * cp, cy * cp, sp])
    right0 = np.array([cy, -sy, 0.0])
    up0 = np.cross(right0, fwd)
    return np.column_stack([cr * right0 - sr * up0, sr * right0 + cr * up0, fwd])


def camera_to_body(yaw, pitch, roll):
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    fwd = np.array([sy * cp, sp, cy * cp])
    right = np.array([cy, 0.0, -sy])
    up = np.cross(fwd, right)
    if roll:
        cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
        right, up = cr * right - sr * up, sr * right + cr * up
    return np.column_stack([right, up, fwd])


def msfs_hwnd():
    k32 = ctypes.windll.kernel32
    best = [None, 0]

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        if not user32.IsWindowVisible(h):
            return True
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        hp = k32.OpenProcess(0x1000, False, pid.value)
        if not hp:
            return True
        buf = ctypes.create_unicode_buffer(520)
        n = wintypes.DWORD(520)
        ok = k32.QueryFullProcessImageNameW(hp, 0, buf, ctypes.byref(n))
        k32.CloseHandle(hp)
        if ok and buf.value.lower().endswith(("flightsimulator2024.exe", "flightsimulator.exe")):
            rc = wintypes.RECT()
            user32.GetClientRect(h, ctypes.byref(rc))
            if rc.right * rc.bottom > best[1]:
                best[0], best[1] = h, rc.right * rc.bottom
        return True

    user32.EnumWindows(cb, 0)
    return best[0]


def client_rect(h):
    rc = wintypes.RECT()
    user32.GetClientRect(h, ctypes.byref(rc))
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(h, ctypes.byref(pt))
    return pt.x, pt.y, pt.x + rc.right, pt.y + rc.bottom


def bring_front(h):
    user32.ShowWindow(h, 9)  # SW_RESTORE
    # a background process may not take the foreground; the ALT key trick makes Windows allow it
    user32.keybd_event(0x12, 0, 0, 0)
    user32.SetForegroundWindow(h)
    user32.keybd_event(0x12, 0, 2, 0)
    time.sleep(0.5)
    return user32.GetForegroundWindow() == h


class WindowGrabber:
    """Frames of the MSFS window itself via Windows Graphics Capture (the API OBS window capture uses): works while
    other windows cover it (not when minimised) and never sees anything but MSFS. Needs `pip install windows-capture`
    (run this script without -I so the user site-packages are found)."""

    def __init__(self, hwnd):
        from windows_capture import WindowsCapture  # noqa: E402
        import threading
        self.hwnd = hwnd
        self.lock = threading.Lock()
        self.frame, self.n = None, 0
        cap = WindowsCapture(cursor_capture=False, draw_border=False, window_hwnd=hwnd)

        @cap.event
        def on_frame_arrived(frame, control):
            buf = np.array(frame.frame_buffer[:, :, :3][:, :, ::-1], dtype=np.uint8)  # BGRA -> RGB copy
            with self.lock:
                self.frame, self.n = buf, self.n + 1

        @cap.event
        def on_closed():
            pass

        self.control = cap.start_free_threaded()

    def grab(self, timeout=10.0):
        """Newest frame that arrived after this call (so it shows the current state), cropped to the client area."""
        with self.lock:
            n0 = self.n
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                if self.n > n0 + 1 and self.frame is not None:
                    img = self.frame
                    break
            time.sleep(0.02)
        else:
            raise RuntimeError("no new frame from the MSFS window (minimised?)")
        # window frame -> client area. The captured frame is the visible window (DWM extended frame bounds), not
        # GetWindowRect, which includes the invisible resize borders (7 px on each side of a normal window: cropping
        # with it shifted the picture 7 px sideways = 0.5 deg of yaw in an 800 px window)
        wr = wintypes.RECT()
        if ctypes.windll.dwmapi.DwmGetWindowAttribute(self.hwnd, 9, ctypes.byref(wr), ctypes.sizeof(wr)) != 0:
            user32.GetWindowRect(self.hwnd, ctypes.byref(wr))
        cl = client_rect(self.hwnd)
        if img.shape[1] != wr.right - wr.left or img.shape[0] != wr.bottom - wr.top:
            raise RuntimeError(f"captured frame {img.shape[1]}x{img.shape[0]} does not match the window "
                               f"{wr.right - wr.left}x{wr.bottom - wr.top}")
        ox, oy = cl[0] - wr.left, cl[1] - wr.top
        w, h = cl[2] - cl[0], cl[3] - cl[1]
        return img[oy:oy + h, ox:ox + w].astype(np.float32)

    def stop(self):
        try:
            self.control.stop()
        except Exception:  # noqa: BLE001
            pass


GRABBER = {"g": None}


def grab(_rect=None):
    return GRABBER["g"].grab()


def wait_picture(rect, log, stable_n=4, thresh=0.8, timeout=60.0):
    """Wait until consecutive screenshots (downscaled grey) differ by less than `thresh` levels on average."""
    t0, prev, ok = time.time(), None, 0
    while time.time() - t0 < timeout:
        img = Image.fromarray(grab(rect).astype(np.uint8)).convert("L").resize((430, 180))
        a = np.asarray(img, dtype=np.float32)
        if prev is not None:
            d = float(np.abs(a - prev).mean())
            ok = ok + 1 if d < thresh else 0
            if ok >= stable_n:
                log(f"  picture steady after {time.time() - t0:.0f} s")
                return True
        prev = a
        time.sleep(0.5)
    log("  picture did not settle in time")
    return False


D_CLOCK, E_ZULU_H, E_ZULU_M = 31, 21, 22


class CamSim(C.Sim):
    def __init__(self, dll):
        super().__init__(dll)
        for n, u in CAM_VARS:
            self.sc.add_var(D_CAM, n, u)
        self.sc.add_var(D_CLOCK, "ZULU TIME", "seconds")
        self.sc.map_event(E_ZULU_H, "ZULU_HOURS_SET")
        self.sc.map_event(E_ZULU_M, "ZULU_MINUTES_SET")

    def zulu_s(self, timeout=2.0):
        self.req += 1
        rid = self.req
        self.sc.request(rid, D_CLOCK, 1)
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.sc.poll():
                if m[0] == "data" and m[1] == rid:
                    return struct.unpack_from("<d", m[3])[0]
            time.sleep(0.005)
        raise TimeoutError("no clock data")

    def set_zulu(self, seconds):
        """Same day, new time of day (ZULU_HOURS_SET / ZULU_MINUTES_SET work in MSFS 2024, checked 11/15)."""
        m = int(seconds // 60) % (24 * 60)
        self.sc.transmit(E_ZULU_H, m // 60)
        self.sc.transmit(E_ZULU_M, m % 60)

    def place_pitched(self, lat, lon, alt, hdg, pitch_down):
        self.sc.set_data(C.D_POS, [lat, lon, alt, hdg, pitch_down, 0.0])

    def cam(self, timeout=2.0):
        self.req += 1
        rid = self.req
        self.sc.request(rid, D_CAM, 1)
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.sc.poll():
                if m[0] == "data" and m[1] == rid:
                    return dict(zip([n for n, _ in CAM_VARS], struct.unpack_from(f"<{len(CAM_VARS)}d", m[3])))
            time.sleep(0.005)
        raise TimeoutError("no camera data")


def project(cam, W, H, pts_enu):
    """ENU points (relative to the aircraft datum lat/lon, z = altitude m) -> pixel x, y, valid."""
    R_ac = aircraft_to_enu(cam["PLANE HEADING DEGREES TRUE"], -cam["PLANE PITCH DEGREES"], -cam["PLANE BANK DEGREES"])
    R = R_ac @ camera_to_body(cam["L:p42_cp_r_yaw"], cam["L:p42_cp_r_pitch"], cam["L:p42_cp_r_roll"])
    # lateral: a_x = eyepoint anchor (+ left) + view offset r_x (+ right) -> eye at 2 r_x - a_x (Birdie core/geo.h
    # eye_lateral_m); stations saved before r_x was recorded use a_x as is
    ex = 2 * cam["L:p42_cp_r_x"] - cam["L:p42_cp_a_x"] if "L:p42_cp_r_x" in cam else cam["L:p42_cp_a_x"]
    eye = np.array([ex, cam["L:p42_cp_a_y"], cam["L:p42_cp_a_z"]])
    cam_pos = np.array([0.0, 0.0, cam["PLANE ALTITUDE"]]) + R_ac @ eye
    f = focal_px(cam["L:p42_cp_r_zoom"], H)
    r = (pts_enu - cam_pos) @ R  # rows: R^T (p - c)
    z = r[:, 2]
    ok = z > 1.0
    x = W / 2 + f * r[:, 0] / np.where(ok, z, 1)
    y = H / 2 - f * r[:, 1] / np.where(ok, z, 1)
    return x, y, ok & (x >= 0) & (x < W - 1) & (y >= 0) & (y < H - 1)


def bilinear(img, x, y):
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    a, b = img[y0, x0], img[y0, x0 + 1]
    c, d = img[y0 + 1, x0], img[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def measure_cut(sim, rw, along, a, rect, log, outdir, tag):
    h = math.radians(rw["HEADING"])
    u, rr = np.array([math.sin(h), math.cos(h)]), np.array([math.cos(h), -math.sin(h)])
    lat0, lon0 = rw["LATITUDE"], rw["LONGITUDE"]

    def latlon(al, cr):
        e, n = u * al + rr * cr
        return C.offset(lat0, lon0, e, n)

    lat, lon = latlon(along, 0.0)
    g = C.wait_scenery(sim, lat, lon, rw["HEADING"], log)
    if g is None:
        return None
    for _ in range(4):
        sim.place_pitched(lat, lon, g + a.height, rw["HEADING"], a.pitch)
        time.sleep(0.25)
    wait_picture(rect, log)
    cam = sim.cam()
    img = grab(rect)
    # mask shot: 6 m to the right; the cockpit does not move in the picture, the ground does
    lat2, lon2 = latlon(along, 6.0)
    for _ in range(4):
        sim.place_pitched(lat2, lon2, g + a.height, rw["HEADING"], a.pitch)
        time.sleep(0.25)
    wait_picture(rect, log, stable_n=2)
    img2 = grab(rect)
    for _ in range(3):
        sim.place_pitched(lat, lon, g + a.height, rw["HEADING"], a.pitch)
        time.sleep(0.2)
    Hh, Ww = img.shape[:2]
    grey = img.mean(axis=2)
    diff = np.abs(grey - img2.mean(axis=2))
    # dilate "moving" pixels a little: anything within 3 px of change counts as scenery
    moving = diff > 6
    from scipy.ndimage import binary_dilation  # noqa: E402
    moving = binary_dilation(moving, iterations=3)

    # ortho strip around the aircraft position, in facility runway coordinates (along, cross)
    half_c = rw["WIDTH"] / 2 + 30
    cs = np.arange(-half_c, half_c + 1e-9, 0.2)
    als = np.arange(-a.strip, a.strip + 1e-9, 0.5)
    A, Cc = np.meshgrid(als, cs, indexing="ij")
    e = (u[0] * (along + A) + rr[0] * Cc).ravel()
    n = (u[1] * (along + A) + rr[1] * Cc).ravel()
    # relative to the aircraft datum
    de, dn = C.enu(lat0, lon0, cam["PLANE LATITUDE"], cam["PLANE LONGITUDE"])
    pts = np.column_stack([e - de, n - dn, np.full(e.shape, cam["GROUND ALTITUDE"])])
    x, y, ok = project(cam, Ww, Hh, pts)
    vals = np.full((e.size, 3), np.nan, dtype=np.float32)
    vals[ok] = bilinear(img, x[ok], y[ok])
    mv = np.zeros(e.size, bool)
    mv[ok] = moving[y[ok].astype(int), x[ok].astype(int)]
    vals[~mv] = np.nan
    ortho = vals.reshape(len(als), len(cs), 3)
    lum = np.nanmean(ortho, axis=2)
    cover = np.isfinite(lum).mean()
    log(f"  cut {tag}: ortho {lum.shape[0]}x{lum.shape[1]}, {100 * cover:.0f} % of the strip visible")

    # profiles across the runway
    p90 = np.nanpercentile(lum, 90, axis=0)
    p50 = np.nanmedian(lum, axis=0)
    W0 = rw["WIDTH"]
    best = None
    for w in np.arange(W0 - 8, W0 + 8.01, 0.2):
        hw = w / 2
        for d in np.arange(-30, 30.01, 0.1):
            il, ir = np.searchsorted(cs, d - hw), np.searchsorted(cs, d + hw)
            if il < 2 or ir > len(cs) - 3:
                continue
            s = np.nanmax(p90[il - 2:il + 3]) + np.nanmax(p90[ir - 2:ir + 3]) - 2 * np.nanmedian(p90)
            if np.isfinite(s) and (best is None or s > best[0]):
                best = (s, d, w)
    # heading: each half of the strip on its own, with the width fixed
    halves = []
    if best:
        for sl in (slice(0, len(als) // 2), slice(len(als) // 2, None)):
            q = np.nanpercentile(lum[sl], 90, axis=0)
            hb = None
            for d in np.arange(best[1] - 5, best[1] + 5.01, 0.1):
                il, ir = np.searchsorted(cs, d - best[2] / 2), np.searchsorted(cs, d + best[2] / 2)
                if il < 2 or ir > len(cs) - 3:
                    continue
                s = np.nanmax(q[il - 2:il + 3]) + np.nanmax(q[ir - 2:ir + 3])
                if np.isfinite(s) and (hb is None or s > hb[0]):
                    hb = (s, d)
            halves.append((float(np.mean(als[sl])), hb[1] if hb else None))

    # pictures to check by eye: ortho (rows = along, up = forward) with the fit and the facility edges
    vis = np.nan_to_num(ortho[::-1], nan=40).clip(0, 255).astype(np.uint8)
    im = Image.fromarray(vis).resize((vis.shape[1] * 2, vis.shape[0] * 2), Image.NEAREST)
    dr = ImageDraw.Draw(im)

    def col(c):
        return int((c + half_c) / 0.2 * 2)

    for c in (-W0 / 2, W0 / 2):
        dr.line([(col(c), 0), (col(c), im.height)], fill=(255, 60, 60), width=1)  # facility edges: red
    if best:
        for c in (best[1] - best[2] / 2, best[1] + best[2] / 2, best[1]):
            dr.line([(col(c), 0), (col(c), im.height)], fill=(60, 255, 60), width=1)  # fit: green
    im.save(os.path.join(outdir, f"{tag}_ortho.png"))
    pw, ph = 900, 300
    pim = Image.new("RGB", (pw, ph), (20, 20, 20))
    pd = ImageDraw.Draw(pim)
    for prof, colr in ((p90, (255, 255, 255)), (p50, (120, 160, 255))):
        pts2 = [(i / len(cs) * pw, ph - 10 - (0 if not np.isfinite(v) else v) / 255 * (ph - 20)) for i, v in enumerate(prof)]
        pd.line(pts2, fill=colr, width=1)
    for c, colr in ((-W0 / 2, (255, 60, 60)), (W0 / 2, (255, 60, 60))):
        pd.line([((c + half_c) / (2 * half_c) * pw, 0), ((c + half_c) / (2 * half_c) * pw, ph)], fill=colr)
    if best:
        for c in (best[1] - best[2] / 2, best[1] + best[2] / 2):
            pd.line([((c + half_c) / (2 * half_c) * pw, 0), ((c + half_c) / (2 * half_c) * pw, ph)], fill=(60, 255, 60))
    pim.save(os.path.join(outdir, f"{tag}_profile.png"))
    Image.fromarray(img.astype(np.uint8)).resize((Ww // 2, Hh // 2)).save(os.path.join(outdir, f"{tag}_shot.jpg"))
    if not best:
        return None
    return {"along": float(along), "centre_cross": float(best[1]), "width": float(best[2]), "score": float(best[0]),
            "halves": [(float(x), None if d is None else float(d)) for x, d in halves], "coverage": float(cover),
            "ground_alt": cam["GROUND ALTITUDE"], "zoom": cam["L:p42_cp_r_zoom"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("icaos", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--height", type=float, default=120.0)
    ap.add_argument("--pitch", type=float, default=80.0, help="nose down, deg")
    ap.add_argument("--strip", type=float, default=60.0, help="half length of the rectified strip, m")
    ap.add_argument("--min-length", type=float, default=1200)
    ap.add_argument("--daylight", action="store_true", help="set the sim clock to local solar noon per airport "
                                                              "(put back at the end)")
    ap.add_argument("--dll", default=os.path.join(HERE, "..", "..", "build", "release", "SimConnect.dll"))
    ap.add_argument("--oa", default=os.path.join(HERE, "data", "runways.csv"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(a.out, "topdown.log"), "a", encoding="utf-8")

    def log(s):
        print(s, flush=True)
        logf.write(s + "\n")
        logf.flush()

    h = msfs_hwnd()
    if not h:
        sys.exit("MSFS window not found")
    rect = client_rect(h)
    GRABBER["g"] = WindowGrabber(h)
    oa = C.oa_load(a.oa)
    sim = CamSim(os.path.abspath(a.dll))
    home = sim.read()
    cam0 = sim.cam()
    if cam0["L:p42_cp_r_zoom"] <= 0.05 or cam0["L:p42_cp_mode"] >= 0.5:
        sys.exit("ChasePlane cockpit view needed (zoom %.3f, mode %.0f)" % (cam0["L:p42_cp_r_zoom"], cam0["L:p42_cp_mode"]))
    zulu0, t_zulu0 = sim.zulu_s(), time.time()
    log(f"start {home['PLANE LATITUDE']:.6f},{home['PLANE LONGITUDE']:.6f}; client {rect}; zoom {cam0['L:p42_cp_r_zoom']:.3f}"
        f"; zulu {zulu0 / 3600:.2f} h")
    sim.sc.transmit(C.E_SLEW_ON)
    time.sleep(0.3)
    sim.sc.transmit(C.E_PAUSE_OFF)
    results = []
    try:
        for icao in a.icaos:
            rws = sim.runways(icao)
            if a.daylight and rws:
                noon = (12.0 - rws[0]["LONGITUDE"] / 15.0) % 24.0
                sim.set_zulu(noon * 3600)
                log(f"{icao}: clock set to local solar noon ({noon:.1f} Z)")
            for rw in rws:
                if rw["LENGTH"] < a.min_length:
                    continue
                name = f"{C.rwy_name(rw['PRIMARY_NUMBER'], rw['PRIMARY_DESIGNATOR'])}/" \
                       f"{C.rwy_name(rw['SECONDARY_NUMBER'], rw['SECONDARY_DESIGNATOR'])}"
                log(f"{icao} {name}: facility {rw['LATITUDE']:.6f},{rw['LONGITUDE']:.6f} hdg {rw['HEADING']:.2f} "
                    f"{rw['LENGTH']:.0f}x{rw['WIDTH']:.0f}")
                cuts = []
                for k, frac in enumerate((-0.35, 0.0, 0.35)):
                    tag = f"{icao}_{name.replace('/', '-')}_{k}"
                    s = measure_cut(sim, rw, frac * rw["LENGTH"], a, rect, log, a.out, tag)
                    if s:
                        hv = ", ".join(f"{al:+.0f}:{d:+.2f}" for al, d in s["halves"] if d is not None)
                        log(f"   along {s['along']:+6.0f}: centre {s['centre_cross']:+.2f} m, width {s['width']:.1f} m "
                            f"(halves {hv}), coverage {100 * s['coverage']:.0f} %")
                        cuts.append(s)
                res = {"icao": icao, "runway": name, "facility": rw, "cuts": cuts}
                r = C.oa_match(oa, icao, C.rwy_name(rw["PRIMARY_NUMBER"], rw["PRIMARY_DESIGNATOR"]),
                               C.rwy_name(rw["SECONDARY_NUMBER"], rw["SECONDARY_DESIGNATOR"]))
                line = C.oa_line(r) if r else None
                if line and cuts:
                    hh = math.radians(rw["HEADING"])
                    (la1, lo1), (la2, lo2) = line
                    e1, n1 = C.enu(rw["LATITUDE"], rw["LONGITUDE"], la1, lo1)
                    e2, n2 = C.enu(rw["LATITUDE"], rw["LONGITUDE"], la2, lo2)
                    a1, c1 = e1 * math.sin(hh) + n1 * math.cos(hh), e1 * math.cos(hh) - n1 * math.sin(hh)
                    a2, c2 = e2 * math.sin(hh) + n2 * math.cos(hh), e2 * math.cos(hh) - n2 * math.sin(hh)
                    oa_c = [c1 + (c["along"] - a1) / (a2 - a1) * (c2 - c1) for c in cuts]
                    res["ourairports_cross"] = oa_c
                    log("   picture minus facility: " + ", ".join(f"{c['centre_cross']:+.2f}" for c in cuts) +
                        "  | picture minus OurAirports: " +
                        ", ".join(f"{c['centre_cross'] - o:+.2f}" for c, o in zip(cuts, oa_c)))
                results.append(res)
                with open(os.path.join(a.out, "results.json"), "w", encoding="utf-8") as f:
                    json.dump(results, f, indent=1)
    finally:
        for _ in range(8):
            sim.place_pitched(home["PLANE LATITUDE"], home["PLANE LONGITUDE"], home["PLANE ALTITUDE"],
                              home["PLANE HEADING DEGREES TRUE"], 0.0)
            time.sleep(0.5)
        if a.daylight:
            sim.set_zulu(zulu0 + (time.time() - t_zulu0))
            log("clock put back")
        sim.sc.transmit(C.E_PAUSE_ON)
        time.sleep(0.3)
        if home["IS SLEW ACTIVE"] < 0.5:
            sim.sc.transmit(C.E_SLEW_OFF)
        log("aircraft put back, sim paused")
        sim.sc.close()
        GRABBER["g"].stop()


if __name__ == "__main__":
    main()
