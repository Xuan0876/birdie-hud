"""Build a parity-test fixture from a recorded session, using the Python reference implementation.

A fixture is everything the C++ port needs to prove it draws the same HUD, without the simulator:

  input.csv      the exact SimConnect state stream fed to the model (columns = reference VARS keys, plus t in s),
                 30 Hz, already derived the way the live app sees it (see "derived fields" below)
  fixture.json   window size, overlay box, settings, runway, check times, provenance
  golden/NNN.json   at each check time: frame geometry (bird pixel, focal, runway polygon, flare state) and the
                    full renderer command list (hud.commands(): floats + string pool)
  golden/NNN.png    the same frame rendered by hudgfx.dll into memory (RGBA, premultiplied alpha), box-sized
  frames/NNN.jpg    the recording frame at that moment (full client size), for eyeballing
  preview/NNN.jpg   frame + HUD composited (box crop), for eyeballing

Derived fields (the recording logger did not log them, the live app reads them from SimConnect):
  vy_fps = d(alt_ft)/dt over +-0.25 s (stands in for VELOCITY WORLD Y), radio_alt_ft = agl_ft, baro_mb = 1013,
  mach = ias_kt / 661.5, beta_deg = 0.
Determinism: the flare cue's blink phase uses the replay time instead of time.perf_counter().

    python tests/make_golden.py <session_dir> --name lax_dusk_07L --start 285 --end 392 --lag -0.38 \
        --runway "33.9376997,-118.4011429,26.6,82.97,3388,45,07L" --checks 290,320,350,370,380,384,386,388,390
"""
import argparse
import csv
import json
import math
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(os.path.dirname(HERE), "reference", "python")
sys.path.insert(0, REF)
sys.path.insert(0, os.path.join(REF, "tools"))

import overlay as O  # noqa: E402
import runways as RW  # noqa: E402
from obs_timeline import recordings_from_obs  # noqa: E402

# settings frozen into the fixture (the reference DEFAULTS at the time of writing, HUD size as used in flight)
SETTINGS = dict(O.DEFAULTS, hud_scale=0.75, show_fd=False, opacity=1.0, ra_offset_ft=0.0, ra_offset_note="")
KEYS = [k for k, _n, _u in O.VARS]


def load_rows(sdir):
    with open(os.path.join(sdir, "track.csv"), newline="") as f:
        rows = [{k: float(v) for k, v in r.items() if v not in ("", None)} for r in csv.DictReader(f)]
    return rows


def interp(rows, ts, t):
    import bisect
    i = max(1, min(len(rows) - 1, bisect.bisect_left(ts, t)))
    a, b = rows[i - 1], rows[i]
    w = 0.0 if b["t"] == a["t"] else (t - a["t"]) / (b["t"] - a["t"])
    out = {}
    for k, va in a.items():
        vb = b.get(k, va)
        if k in O.ANGLE_KEYS or k in ("trk_true", "hdg_mag", "wind_dir"):
            d = (vb - va + 180) % 360 - 180
            out[k] = (va + w * d) % 360 if k in ("hdg_true", "trk_true", "hdg_mag", "wind_dir") else va + w * d
        else:
            out[k] = va + w * (vb - va)
    return out


def state_at(rows, ts, t):
    s = interp(rows, ts, t)
    a, b = interp(rows, ts, t - 0.25), interp(rows, ts, t + 0.25)
    s["vy_fps"] = (b["alt_ft"] - a["alt_ft"]) / 0.5
    s["radio_alt_ft"] = s.get("agl_ft", 0.0)
    s["ra_raw_ft"] = s["radio_alt_ft"]
    s["baro_mb"] = 1013.0
    s["mach"] = s.get("ias_kt", 0.0) / 661.5
    s["beta_deg"] = 0.0
    s.setdefault("on_ground", 0.0)
    return {k: s.get(k, 0.0) for k in KEYS} | {"ra_raw_ft": s["ra_raw_ft"]}


def video_frame(path, vs, W, H):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{vs}", "-i", path, "-frames:v", "1", "-f", "rawvideo",
                          "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(H, W, 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    ap.add_argument("--name", required=True)
    ap.add_argument("--start", type=float, required=True, help="video seconds")
    ap.add_argument("--end", type=float, required=True)
    ap.add_argument("--lag", type=float, default=-0.34, help="extra log/video lag, s (foe_check fitlag)")
    ap.add_argument("--runway", help="centre_lat,centre_lon,alt_m,landing_hdg_true,length_m,width_m,name")
    ap.add_argument("--checks", required=True, help="comma-separated video seconds")
    ap.add_argument("--hz", type=float, default=30.0)
    a = ap.parse_args()

    from PIL import Image
    from PyQt6.QtGui import QGuiApplication
    app = QGuiApplication(sys.argv)  # noqa: F841  (hud.build uses QTransform)

    sdir = os.path.abspath(a.session)
    rows = load_rows(sdir)
    ts = [r["t"] for r in rows]
    with open(os.path.join(sdir, "obs.jsonl"), encoding="utf-8") as f:
        rec = recordings_from_obs([json.loads(x) for x in f if x.strip()])[0]
    with open(os.path.join(sdir, "meta.json"), encoding="utf-8") as f:
        meta = json.load(f)
    probe = json.loads(subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                       "stream=width,height", "-of", "json", rec["path"]],
                                      capture_output=True, text=True, check=True).stdout)["streams"][0]
    W, H = probe["width"], probe["height"]
    bw, bh = int(W * SETTINGS["box_w"]), int(H * SETTINGS["box_h"])
    ox, oy = (W - bw) // 2, (H - bh) // 2

    def log_t(vs):
        return (vs - rec["b"]) / rec["a"] + a.lag

    rw = None
    if a.runway:
        v = a.runway.split(",")
        rw = RW.Runway("TEST", *map(float, v[:6]), v[6])
    checks = [float(x) for x in a.checks.split(",")]

    out = os.path.join(HERE, "fixtures", a.name)
    for sub in ("golden", "frames", "preview"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)

    # 1. the input stream
    t0, t1 = log_t(a.start), log_t(a.end)
    n = int((t1 - t0) * a.hz) + 1
    stream = [(t0 + i / a.hz, state_at(rows, ts, t0 + i / a.hz)) for i in range(n)]
    with open(os.path.join(out, "input.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t"] + KEYS + ["ra_raw_ft"])
        for t, s in stream:
            w.writerow([f"{t - t0:.4f}"] + [repr(round(s[k], 6)) for k in KEYS + ["ra_raw_ft"]])

    # 2. replay through the reference model; capture each check time
    import hudgfx
    gfx = hudgfx.HudGfx(-32000, -32000, 16, 16)  # off-screen window, only render_to_memory is used
    model = O.Model(SETTINGS)
    want = sorted((log_t(vs) - t0, vs) for vs in checks)
    clock = {"t": 0.0}
    real_flare = O.flare_cue
    O.flare_cue = lambda m, s, r, st, now=None: real_flare(m, s, r, st, now=clock["t"])
    golden = []
    k = 0
    for t, s in stream:
        rt = t - t0
        clock["t"] = rt
        model.update(s, rt)
        while k < len(want) and rt >= want[k][0] - 1e-9:
            vs = want[k][1]
            idx = f"{k:03d}"
            bird, _line, f_px, rwy_poly, _ex = model.geometry(s, W, H, rw)
            flare = O.flare_cue(model, s, rw, SETTINGS)
            cmds, text = O.hud_commands(model, s, W, H, ox, oy, SETTINGS, rw)
            rec_g = {"index": k, "video_s": vs, "t": round(rt, 4), "state": s,
                     "geometry": {"bird_px": bird, "focal_px": f_px, "runway_px": rwy_poly,
                                  "fpa_deg": model.fpa_deg(), "vs_fpm": model.vs_fpm(), "flare": flare},
                     "commands": [round(c, 4) for c in cmds], "text": text}
            with open(os.path.join(out, "golden", idx + ".json"), "w", encoding="utf-8") as fj:
                json.dump(rec_g, fj, ensure_ascii=False, indent=0)
            hud = gfx.render_to_memory(cmds, text, bw, bh)  # RGBA premultiplied
            Image.fromarray(hud, "RGBA").save(os.path.join(out, "golden", idx + ".png"))
            bg = video_frame(rec["path"], vs, W, H)
            Image.fromarray(bg).save(os.path.join(out, "frames", idx + ".jpg"), quality=90)
            crop = bg[oy:oy + bh, ox:ox + bw].astype(np.float32)
            al = hud[:, :, 3:4].astype(np.float32) / 255.0
            comp = hud[:, :, :3].astype(np.float32) + crop * (1 - al)  # premultiplied over
            Image.fromarray(np.clip(comp, 0, 255).astype(np.uint8)).save(
                os.path.join(out, "preview", idx + ".jpg"), quality=88)
            golden.append({"index": k, "video_s": vs, "t": round(rt, 4), "agl_ft": round(s["agl_ft"], 1),
                           "bird_px": bird, "flare": flare and flare["n"]})
            print(f"{idx}  video {vs:6.1f}s  t {rt:7.3f}  agl {s['agl_ft']:6.0f} ft  bird {bird}  "
                  f"{len(cmds)} floats")
            k += 1
    gfx.close()

    fx = {
        "name": a.name,
        "aircraft": meta.get("aircraft_title", "").split(" - ")[0],
        "camera": "ChasePlane fixed cockpit view (cp_mode 0)",
        "client_px": [W, H], "box_px": [ox, oy, bw, bh],
        "input": {"file": "input.csv", "hz": a.hz, "t": "seconds from the first row",
                  "derived": "vy_fps from altitude difference, radio_alt_ft = agl_ft, baro 1013, mach ias/661.5, beta 0"},
        "runway": (dict(zip(["lat", "lon", "alt_m", "landing_hdg_true", "length_m", "width_m", "name"],
                            a.runway.split(","))) | {"source": "OpenStreetMap (sim has its own data live)"})
        if a.runway else None,
        "settings": SETTINGS,
        "flare_blink_clock": "replay time t (live app: time.perf_counter)",
        "checks": golden,
        "provenance": {"recorded": meta.get("started_local", "")[:10], "video_window_s": [a.start, a.end],
                       "log_video_lag_s": a.lag, "generator": "tests/make_golden.py (reference/python)"},
        "compare": {"geometry_tol_px": 0.05, "commands_tol": 1e-3,
                    "image": "D2D anti-aliasing may differ in the last bit: compare with a small per-pixel tolerance"},
    }
    with open(os.path.join(out, "fixture.json"), "w", encoding="utf-8") as f:
        json.dump(fx, f, ensure_ascii=False, indent=1)
    print("->", out)


if __name__ == "__main__":
    main()
