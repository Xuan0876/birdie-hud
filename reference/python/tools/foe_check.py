"""Offline check of the flight-path-vector (FPV, "bird") projection against the picture itself.

The bird is where the aircraft is going. In the video that is the focus of expansion (FOE): once camera rotation is
taken out, every ground feature streams radially away from one point, and that point does not move. This script
finds the FOE from the recording and compares it with the bird projected from the logged data.

Two image estimates per frame pair (t, t + gap):
  A  derotated FOE — track features (LK), undo the camera rotation known from the log (aircraft attitude + ChasePlane
     head angles) with the infinite homography, then RANSAC the point all residual flow radiates from.
  B  pure image — essential matrix from the same tracks (no log data at all), translation direction = FOE.
     It also gives the camera rotation between the frames, which is used to measure the log/video time lag.
Predicted bird: ground velocity (GPS GROUND TRUE TRACK, GROUND VELOCITY, VERTICAL SPEED) rotated into the camera.

    python foe_check.py <session_dir> --start <video s> --dur <s> [--name x] [--lag 0] [--gap 6] [--step 2]

Writes <out>/<name>.csv, <name>_summary.json, <name>_plot.png and <name>.mp4 (overlay at 1920 wide).
"""
import argparse
import bisect
import csv
import json
import math
import os
import subprocess
import sys
from collections import deque

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import cp_camera as C  # noqa: E402
from obs_timeline import recordings_from_obs  # noqa: E402

KT = 0.514444
FPM = 0.00508
FT = 0.3048
W, H = 1920, 804
ANGLES = {"hdg_true", "hdg_mag", "trk_true", "cp_yaw"}
RAD = 180 / math.pi


# ---------------------------------------------------------------- data
def load_track(sdir):
    with open(os.path.join(sdir, "track.csv"), encoding="utf-8") as f:
        rows = [{k: float(v) for k, v in r.items() if v not in ("", None)} for r in csv.DictReader(f)]
    return rows


def interp(rows, ts, t):
    i = bisect.bisect_left(ts, t)
    if i <= 0:
        return dict(rows[0])
    if i >= len(rows):
        return dict(rows[-1])
    a, b = rows[i - 1], rows[i]
    w = (t - a["t"]) / (b["t"] - a["t"]) if b["t"] > a["t"] else 0.0
    out = {}
    for k in a:
        if k not in b:
            continue
        if k in ANGLES:
            out[k] = a[k] + w * ((b[k] - a[k] + 180) % 360 - 180)
        else:
            out[k] = a[k] + w * (b[k] - a[k])
    out["cp_mode"] = a["cp_mode"] if w < 0.5 else b["cp_mode"]
    return out


def cam_R(s):
    """Camera (x right, y up, z fwd) -> ENU."""
    return C.aircraft_rotation(s) @ C.camera_to_body(s["cp_yaw"], s["cp_pitch"], s.get("cp_roll", 0.0))


def focal(s):
    return s["cp_zoom"] * C.F_PER_ZOOM * W / 3440.0


def pix(r, f):
    """Camera ray (y up) -> pixel."""
    return np.array([W / 2 + f * r[0] / r[2], H / 2 - f * r[1] / r[2]])


def rays(p, f):
    """Pixels (N,2) -> camera rays (N,3), y up."""
    return np.column_stack([(p[:, 0] - W / 2) / f, -(p[:, 1] - H / 2) / f, np.ones(len(p))])


def vel_gps(s):
    trk = math.radians(s["trk_true"])
    return np.array([s["gs_kt"] * KT * math.sin(trk), s["gs_kt"] * KT * math.cos(trk), s["vs_fpm"] * FPM])


def vel_pos(rows, ts, t, h=0.5):
    """Velocity from the logged position itself (central difference) — cross-check of the GPS vars."""
    a, b = interp(rows, ts, t - h), interp(rows, ts, t + h)
    e, n = C.enu(a["lat"], a["lon"], b["lat"], b["lon"])
    return np.array([e, n, (b["alt_ft"] - a["alt_ft"]) * FT]) / (2 * h)


def bird(s, v_enu):
    r = cam_R(s).T @ v_enu
    return pix(r, focal(s)) if r[2] > 0 else None


# ---------------------------------------------------------------- image
def mask_for_video():
    # cockpit_mask_fslabs_a321.png (white = outside view) was made from screenshots and is not in the public
    # repository; draw your own for your aircraft and view. Without it the whole picture is tracked.
    path = os.path.join(HERE, "cockpit_mask_fslabs_a321.png")
    if not os.path.exists(path):
        return np.full((H, W), 255, np.uint8)
    m = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    m = cv2.resize(m, (W, H), interpolation=cv2.INTER_NEAREST)
    return cv2.erode((m > 127).astype(np.uint8) * 255, np.ones((25, 25), np.uint8))  # head shake margin


CLAHE = cv2.createCLAHE(clipLimit=3.0, tileGridSize=(16, 8))


def track(g1, g2, mask):
    g1, g2 = CLAHE.apply(g1), CLAHE.apply(g2)
    p1 = cv2.goodFeaturesToTrack(g1, 4000, 0.002, 6, mask=mask, blockSize=7)
    if p1 is None or len(p1) < 30:
        return None, None
    lk = dict(winSize=(21, 21), maxLevel=4, criteria=(cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 30, 0.01))
    p2, st, _ = cv2.calcOpticalFlowPyrLK(g1, g2, p1, None, **lk)
    pb, st2, _ = cv2.calcOpticalFlowPyrLK(g2, g1, p2, None, **lk)
    fb = np.linalg.norm((pb - p1).reshape(-1, 2), axis=1)
    ok = (st.ravel() == 1) & (st2.ravel() == 1) & (fb < 0.4)
    p1, p2 = p1.reshape(-1, 2)[ok], p2.reshape(-1, 2)[ok]
    inside = (p2[:, 0] >= 0) & (p2[:, 0] < W) & (p2[:, 1] >= 0) & (p2[:, 1] < H)
    p1, p2 = p1[inside], p2[inside]
    ok = mask[p2[:, 1].astype(int), p2[:, 0].astype(int)] > 0
    return p1[ok], p2[ok]


def foe_ransac(p, flow, rng, iters=500, min_flow=1.0):
    """Point the flow radiates from (expansion). Returns (foe, inlier mask) or (None, None)."""
    mag = np.linalg.norm(flow, axis=1)
    use = mag > min_flow
    if use.sum() < 20:
        return None, None
    P, F, M = p[use], flow[use], mag[use]
    D = F / M[:, None]
    N = np.column_stack([-D[:, 1], D[:, 0]])
    # direction noise of a track ~ 0.5 px / |flow|, plus 1.5 deg slack
    cos_tol = np.cos(np.arctan(0.5 / M) + math.radians(1.5))
    best, best_in = None, None
    for _ in range(iters):
        i, j = rng.choice(len(P), 2, replace=False)
        A = np.array([N[i], N[j]])
        if abs(np.linalg.det(A)) < 0.05:
            continue
        e = np.linalg.solve(A, [N[i] @ P[i], N[j] @ P[j]])
        rad = P - e
        rn = np.linalg.norm(rad, axis=1) + 1e-9
        cosang = np.sum(rad * D, axis=1) / rn
        inl = cosang > cos_tol
        if best_in is None or inl.sum() > best_in.sum():
            best, best_in = e, inl
    if best_in is None or best_in.sum() < 15:
        return None, None
    for _ in range(4):  # weighted LS refine on inliers, then re-select
        # line-distance error ~ r * 0.5px / |flow|  ->  weight (|flow| / r)^2  (near ground beats far haze)
        r = np.maximum(np.linalg.norm(P[best_in] - best, axis=1), 30.0)
        w = (M[best_in] / r) ** 2
        n, q = N[best_in], P[best_in]
        A = (n * w[:, None]).T @ n
        bvec = (n * w[:, None]).T @ np.sum(n * q, axis=1)
        if best_in.sum() < 15 or abs(np.linalg.det(A)) < 1e-12 * max(1.0, np.trace(A)) ** 2:
            break
        best = np.linalg.solve(A, bvec)
        rad = P - best
        cosang = np.sum(rad * D, axis=1) / (np.linalg.norm(rad, axis=1) + 1e-9)
        best_in = cosang > cos_tol
    if best_in.sum() < 15:
        return None, None
    full = np.zeros(len(p), bool)
    full[np.flatnonzero(use)[best_in]] = True
    return best, full


def foe_essential(p1, p2, f):
    K = np.array([[f, 0, W / 2], [0, f, H / 2], [0, 0, 1.0]])
    if len(p1) < 30:
        return None, None, 0
    E, m = cv2.findEssentialMat(p1, p2, K, cv2.RANSAC, 0.999, 0.7)
    if E is None or E.shape != (3, 3):
        return None, None, 0
    n, R, t, m2 = cv2.recoverPose(E, p1, p2, K, mask=m)
    c = -(R.T @ t).ravel()  # camera-2 centre in camera-1 coords (OpenCV: y down)
    if c[2] <= 0:
        c = -c
    S = np.diag([1.0, -1.0, 1.0])
    rv = cv2.Rodrigues(S @ R @ S)[0].ravel()  # rotation x2 = R x1 in y-up coords
    return np.array([W / 2 + f * c[0] / c[2], H / 2 + f * c[1] / c[2]]), rv, int(n)


# ---------------------------------------------------------------- lag
def fit_lag(sdir, csv_path, span=1.5, min_inl=60):
    """Video-vs-log lag from camera rotation: essential-matrix rotation of each pair (image) against the logged
    rotation over the same pair, shifted by candidate lags. Returns (best lag added to the CSV's lag, table)."""
    rows = load_track(sdir)
    ts = [r["t"] for r in rows]
    with open(csv_path, encoding="utf-8") as f:
        pairs = [r for r in csv.DictReader(f) if r["rvB0"] and int(r["nB"]) >= min_inl]
    img = np.array([[float(r[k]) for k in ("rvB0", "rvB1", "rvB2")] for r in pairs])
    out = []
    for lag in np.arange(-span, span + 1e-9, 0.02):
        d = []
        for r in pairs:
            s1 = interp(rows, ts, float(r["t"]) + lag)
            s2 = interp(rows, ts, float(r["t2"]) + lag)
            d.append(cv2.Rodrigues(cam_R(s2).T @ cam_R(s1))[0].ravel())
        err = np.sqrt(np.mean(np.sum((img - np.array(d)) ** 2, axis=1))) * RAD
        out.append((round(float(lag), 2), float(err)))
    best = min(out, key=lambda x: x[1])
    return best, out, len(pairs)


# ---------------------------------------------------------------- main
def main():
    if len(sys.argv) > 1 and sys.argv[1] == "fitlag":
        best, table, n = fit_lag(sys.argv[2], sys.argv[3])
        print(f"{n} pairs; rms rotation error (deg) vs extra lag:")
        for lag, e in table[::5]:
            print(f"  {lag:+.2f}  {e:.4f}")
        print(f"best extra lag {best[0]:+.2f} s  (rms {best[1]:.4f} deg)")
        return
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    ap.add_argument("--start", type=float, required=True, help="video seconds")
    ap.add_argument("--dur", type=float, required=True)
    ap.add_argument("--rec", type=int, default=0)
    ap.add_argument("--video")
    ap.add_argument("--lag", type=float, default=0.0, help="seconds added to logger time (positive = later data)")
    ap.add_argument("--gap", type=int, default=20, help="frames between the pair")
    ap.add_argument("--step", type=int, default=2, help="analyse every n-th frame")
    ap.add_argument("--name", default="foe")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(HERE), "foe_out"))
    ap.add_argument("--no-video", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    sdir = os.path.abspath(a.session)
    rows = load_track(sdir)
    ts = [r["t"] for r in rows]
    with open(os.path.join(sdir, "obs.jsonl"), encoding="utf-8") as f:
        rec = recordings_from_obs([json.loads(x) for x in f if x.strip()])[a.rec]
    video = a.video or rec["path"]
    probe = json.loads(subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                       "stream=r_frame_rate", "-of", "json", video],
                                      capture_output=True, text=True).stdout)["streams"][0]
    num, den = map(int, probe["r_frame_rate"].split("/"))
    fps = num / den
    to_t = lambda vs: (vs - rec["b"]) / rec["a"] + a.lag  # noqa: E731

    dec = subprocess.Popen(["ffmpeg", "-v", "error", "-ss", f"{a.start}", "-i", video, "-t", f"{a.dur}", "-f",
                            "rawvideo", "-pix_fmt", "bgr24", "-vf", f"scale={W}:{H}", "-"], stdout=subprocess.PIPE)
    enc = None
    if not a.no_video:
        enc = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{W}x{H}",
                                "-r", f"{fps / a.step}", "-i", "-", "-c:v", "libx264", "-preset", "veryfast", "-crf",
                                "20", "-pix_fmt", "yuv420p", os.path.join(a.out, a.name + ".mp4")], stdin=subprocess.PIPE)
    mask = mask_for_video()
    mask_edge = cv2.morphologyEx(mask, cv2.MORPH_GRADIENT, np.ones((3, 3), np.uint8)) > 0
    rng = np.random.default_rng(0)
    buf = deque(maxlen=a.gap + 1)
    out_rows = []
    nbytes = W * H * 3
    n = -1
    while True:
        raw = dec.stdout.read(nbytes)
        if len(raw) < nbytes:
            break
        n += 1
        buf.append((n, np.frombuffer(raw, np.uint8).reshape(H, W, 3)))
        if len(buf) <= a.gap:
            continue
        n1, im1 = buf[0]
        if n1 % a.step:
            continue
        n2, im2 = buf[-1]
        vs1, vs2 = a.start + n1 / fps, a.start + n2 / fps
        t1, t2 = to_t(vs1), to_t(vs2)
        s1, s2 = interp(rows, ts, t1), interp(rows, ts, t2)
        f = focal(s1)
        g1 = cv2.cvtColor(im1, cv2.COLOR_BGR2GRAY)
        g2 = cv2.cvtColor(im2, cv2.COLOR_BGR2GRAY)
        p1, p2 = track(g1, g2, mask)

        # predicted bird at the pair midpoint direction, expressed in camera 1 (what the FOE of this pair means)
        tm = 0.5 * (t1 + t2)
        sm = interp(rows, ts, tm)
        R1 = cam_R(s1)
        v_gps, v_pos = vel_gps(sm), vel_pos(rows, ts, tm)
        b_gps = pix(R1.T @ v_gps, f) if (R1.T @ v_gps)[2] > 0 else None
        b_pos = pix(R1.T @ v_pos, f) if (R1.T @ v_pos)[2] > 0 else None

        foeA = foeB = rvB = None
        inl = None
        nA = nB = 0
        if p1 is not None and len(p1) >= 30:
            M = R1.T @ cam_R(s2)  # camera-2 ray -> camera-1 ray (points at infinity)
            r1 = (M @ rays(p2, f).T).T
            p2d = np.column_stack([W / 2 + f * r1[:, 0] / r1[:, 2], H / 2 - f * r1[:, 1] / r1[:, 2]])
            foeA, inl = foe_ransac(p1, p2d - p1, rng)
            nA = int(inl.sum()) if inl is not None else 0
            foeB, rvB, nB = foe_essential(p1, p2, f)
        # data rotation of the pair (for the lag fit): x2 = Rd x1
        rvD = cv2.Rodrigues(cam_R(s2).T @ R1)[0].ravel()

        def deg(p, ref):
            if p is None or ref is None:
                return (None, None)
            return (math.degrees(math.atan((p[0] - W / 2) / f)) - math.degrees(math.atan((ref[0] - W / 2) / f)),
                    -(math.degrees(math.atan((p[1] - H / 2) / f)) - math.degrees(math.atan((ref[1] - H / 2) / f))))

        row = {"vs": vs1, "t": t1, "t2": t2, "agl_ft": sm.get("agl_ft"), "bank": sm["bank_deg"], "gs": sm["gs_kt"],
               "vsi": sm["vs_fpm"], "n_tracks": 0 if p1 is None else len(p1), "nA": nA, "nB": nB,
               "bird_x": None if b_gps is None else b_gps[0], "bird_y": None if b_gps is None else b_gps[1],
               "birdpos_x": None if b_pos is None else b_pos[0], "birdpos_y": None if b_pos is None else b_pos[1],
               "A_x": None if foeA is None else foeA[0], "A_y": None if foeA is None else foeA[1],
               "B_x": None if foeB is None else foeB[0], "B_y": None if foeB is None else foeB[1]}
        row["A_dx_deg"], row["A_dy_deg"] = deg(foeA, b_gps)
        row["B_dx_deg"], row["B_dy_deg"] = deg(foeB, b_gps)
        row["pos_dx_deg"], row["pos_dy_deg"] = deg(b_pos, b_gps)
        for k, v in zip(("rvB0", "rvB1", "rvB2"), rvB if rvB is not None else (None,) * 3):
            row[k] = v
        for k, v in zip(("rvD0", "rvD1", "rvD2"), rvD):
            row[k] = v
        out_rows.append(row)

        if enc:
            vis = im1.copy()
            vis[mask_edge] = (vis[mask_edge] * 0.4 + np.array([90, 90, 90]) * 0.6).astype(np.uint8)
            if inl is not None:
                for q, r_, ok in zip(p1, p2d, inl):
                    if ok:
                        e = q + (r_ - q) * 3
                        cv2.line(vis, tuple(np.int32(q)), tuple(np.int32(e)), (0, 200, 255), 1, cv2.LINE_AA)
            # glide reference -3 deg below the horizon, across the track direction
            Rm = R1.T
            trk = math.radians(sm["trk_true"])
            pts = []
            for daz in np.linspace(-12, 12, 25):
                az = trk + math.radians(daz)
                d = np.array([math.sin(az) * math.cos(math.radians(-3)), math.cos(az) * math.cos(math.radians(-3)),
                              math.sin(math.radians(-3))])
                r = Rm @ d
                if r[2] > 0:
                    pts.append(pix(r, f))
            if len(pts) > 1:
                cv2.polylines(vis, [np.int32(pts)], False, (255, 220, 120), 1, cv2.LINE_AA)
            if b_gps is not None:
                x, y = np.int32(b_gps)
                cv2.circle(vis, (x, y), 10, (80, 255, 80), 2, cv2.LINE_AA)
                cv2.line(vis, (x - 26, y), (x - 10, y), (80, 255, 80), 2, cv2.LINE_AA)
                cv2.line(vis, (x + 10, y), (x + 26, y), (80, 255, 80), 2, cv2.LINE_AA)
                cv2.line(vis, (x, y - 10), (x, y - 20), (80, 255, 80), 2, cv2.LINE_AA)
            if foeA is not None:
                x, y = np.int32(foeA)
                cv2.drawMarker(vis, (int(x), int(y)), (255, 0, 255), cv2.MARKER_CROSS, 24, 2, cv2.LINE_AA)
            if foeB is not None:
                x, y = np.int32(foeB)
                cv2.drawMarker(vis, (int(x), int(y)), (255, 255, 0), cv2.MARKER_TILTED_CROSS, 18, 1, cv2.LINE_AA)
            txt = (f"v={vs1:6.2f}s  agl={sm.get('agl_ft', 0):5.0f}ft  tracks={row['n_tracks']} inl={nA}  "
                   f"A-bird: {row['A_dx_deg'] if row['A_dx_deg'] is None else round(row['A_dx_deg'], 2)},"
                   f"{row['A_dy_deg'] if row['A_dy_deg'] is None else round(row['A_dy_deg'], 2)} deg")
            cv2.putText(vis, txt, (12, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 4, cv2.LINE_AA)
            cv2.putText(vis, txt, (12, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
            leg = "green=bird(data)  magenta+=FOE derotated  cyan x=FOE essential  blue line=-3 deg"
            cv2.putText(vis, leg, (12, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 0), 4, cv2.LINE_AA)
            cv2.putText(vis, leg, (12, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (230, 230, 230), 1, cv2.LINE_AA)
            enc.stdin.write(vis.tobytes())
        if len(out_rows) % 100 == 0:
            print(f"  {len(out_rows)} pairs, v={vs1:.1f}s", flush=True)
    dec.wait()
    if enc:
        enc.stdin.close()
        enc.wait()

    with open(os.path.join(a.out, a.name + ".csv"), "w", newline="", encoding="utf-8") as fh:
        wr = csv.DictWriter(fh, fieldnames=list(out_rows[0].keys()))
        wr.writeheader()
        wr.writerows(out_rows)
    print(f"{len(out_rows)} pairs -> {a.out}\\{a.name}.*")


if __name__ == "__main__":
    main()
