"""Zoom -> focal length calibration, step 1: capture the MSFS picture at several ChasePlane zoom levels.

Pause the sim in the ChasePlane cockpit view (do not move the camera), hide the Birdie HUD, then change only the
ChasePlane zoom. Whenever the zoom L-var settles at a new value for 1.5 s (camera angles unchanged), the MSFS client
area is saved as <out>/zoom_<value>.png. Stops after --max captures or --minutes.

    python -I tools/zoom_calib/capture.py --out <folder> [--max 10] [--minutes 6]
"""
import argparse
import ctypes
import os
import struct
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "reference", "python"))
from simconnect_min import SimConnect  # noqa: E402

ctypes.windll.shcore.SetProcessDpiAwareness(2)  # physical pixels
from PIL import ImageGrab  # noqa: E402

user32 = ctypes.windll.user32


def msfs_client_rect():
    """Screen rect (l, t, r, b) of the biggest visible window of FlightSimulator2024.exe."""
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
            area = rc.right * rc.bottom
            if area > best[1]:
                pt = wintypes.POINT(0, 0)
                user32.ClientToScreen(h, ctypes.byref(pt))
                best[0] = (pt.x, pt.y, pt.x + rc.right, pt.y + rc.bottom)
                best[1] = area
        return True

    user32.EnumWindows(cb, 0)
    return best[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--max", type=int, default=10)
    ap.add_argument("--minutes", type=float, default=6)
    ap.add_argument("--dll", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build",
                                                  "release", "SimConnect.dll"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rect = msfs_client_rect()
    if not rect:
        sys.exit("MSFS window not found")
    print(f"MSFS client {rect[2] - rect[0]}x{rect[3] - rect[1]} @ ({rect[0]},{rect[1]})", flush=True)
    sc = SimConnect("birdie_zoom_calib", os.path.abspath(a.dll))
    for n in ("L:p42_cp_r_zoom", "L:p42_cp_r_yaw", "L:p42_cp_r_pitch", "L:p42_cp_r_roll"):
        sc.add_var(1, n, "number")
    sc.request(1, 1, 3)
    cur, since, done, captured = None, 0.0, set(), 0
    t_end = time.time() + a.minutes * 60
    print("ready: change the ChasePlane zoom; each new value is captured after 1.5 s", flush=True)
    while time.time() < t_end and captured < a.max:
        for m in sc.poll():
            if m[0] != "data":
                continue
            z, yaw, pitch, roll = struct.unpack_from("<4d", m[3])
            key = (round(z, 3), round(yaw, 2), round(pitch, 2), round(roll, 2))
            if key != cur:
                cur, since = key, time.time()
        if cur and time.time() - since > 1.5 and cur[0] not in done and cur[0] > 0.05:
            img = ImageGrab.grab(bbox=rect, all_screens=True)
            path = os.path.join(a.out, f"zoom_{cur[0]:.3f}.png")
            img.save(path)
            done.add(cur[0])
            captured += 1
            print(f"captured zoom {cur[0]:.3f} (yaw {cur[1]:+.2f} pitch {cur[2]:+.2f} roll {cur[3]:+.2f}) -> {path}",
                  flush=True)
        time.sleep(0.05)
    sc.close()
    print(f"done, {captured} captures", flush=True)


if __name__ == "__main__":
    main()
