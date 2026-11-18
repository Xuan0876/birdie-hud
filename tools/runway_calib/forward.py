"""Runway position from the cockpit picture: the aircraft stands still on the facility centreline (no taxiing), the
ChasePlane cockpit view looks down the runway, and the painted runway in the screenshot is measured with Birdie's own
camera model. Ground truth for "where is the runway the sim draws", to compare the MSFS facility data with OurAirports.

Why not top-down (topdown.py): an aircraft held in the air needs slew mode, and in slew mode ChasePlane switches to
an external camera (its L-vars then no longer describe the picture); forcing the cockpit view back gives a stabilised
"human" head that does not follow an 80 deg nose-down attitude. On the ground, without slew, the cockpit view is
normal and exactly what Birdie draws over.

Per station (three places along each runway, facing the primary heading):
  1. INITPOSITION on the facility centreline, on the ground; parking brake set; ChasePlane back to the cockpit view
     it had at the start (via the ChasePlane Remote WebSocket).
  2. Wait for the scenery (ground altitude steady, attitude steady, picture steady).
  3. Screenshot of the MSFS window only (Windows Graphics Capture). A second one after moving 4 m on gives the
     cockpit mask (static pixels connected to the picture border).
  4. Rectify the runway ahead (25..220 m from the aircraft) into the facility runway frame: (along, cross) ground
     points -> picture with Birdie's model (attitude x ChasePlane head angles, eye offset, zoom -> vertical FOV),
     ground height from GROUND ALTITUDE and TRUE_SLOPE.
  5. Across-runway profile: the two edge stripes (symmetric pair, width free +-8 m) -> centre offset and width;
     the centreline stripe separately; near half vs far half -> heading difference.
Outputs per station: overlay PNG (facility outline red, measured edges green, on the screenshot), ortho PNG, profile
PNG; numbers in results.json / forward.log. Sign: cross + = right of the facility centreline, looking along the
primary heading.

Needs: cpremote.py (a ChasePlane Remote client, not included in this repository), ChasePlane cockpit view, no ESC menu, MSFS not minimised (may be covered), not flying. MOVES THE USER
AIRCRAFT; puts it back on the ground where it was, restores the parking brake and the clock (--daylight), pauses.

    pip install --user windows-capture websocket-client
    python tools/runway_calib/forward.py ZSNB KASE --out <folder> --daylight      (no -I: user site)
"""
import argparse
import json
import math
import os
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import collect as C  # noqa: E402
import topdown as T  # noqa: E402
try:
    from cpremote import CpRemote  # noqa: E402  (a ChasePlane Remote client; not in the public repository)
except ImportError:
    CpRemote = None
from PIL import Image, ImageDraw  # noqa: E402
from scipy import ndimage  # noqa: E402

D_INIT, D_BRAKE = 40, 41
E_PARK = 23
MIN_QUALITY = 8.0  # weaker edge peak / profile noise; below this the edges are not painted lines (or not seen)


class FwdSim(T.CamSim):
    def __init__(self, dll):
        super().__init__(dll)
        self.sc.add_var(D_INIT, "Initial Position", None, 12)  # SIMCONNECT_DATATYPE_INITPOSITION
        self.sc.add_var(D_BRAKE, "BRAKE PARKING POSITION", "bool")
        self.sc.map_event(E_PARK, "PARKING_BRAKE_SET")

    def init_pos(self, lat, lon, alt_m, hdg, on_ground=True):
        buf = struct.pack("<6dII", lat, lon, alt_m / 0.3048, 0.0, 0.0, hdg, 1 if on_ground else 0, 0)
        self.sc.dll.SimConnect_SetDataOnSimObject(self.sc.h, D_INIT, 0, 0, 0, len(buf), buf)

    def brake(self, timeout=2.0):
        self.req += 1
        rid = self.req
        self.sc.request(rid, D_BRAKE, 1)
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.sc.poll():
                if m[0] == "data" and m[1] == rid:
                    return struct.unpack_from("<d", m[3])[0]
            time.sleep(0.005)
        raise TimeoutError("no brake data")


def settle(sim, log, min_s=8.0, stable_s=4.0, timeout=120.0):
    """Ground altitude, position and attitude steady for `stable_s` (after at least `min_s`: scenery streams in)."""
    t0 = time.time()
    ref, t_ref = None, None
    while time.time() - t0 < timeout:
        c = sim.cam()
        v = np.array([c["GROUND ALTITUDE"], c["PLANE ALTITUDE"], c["PLANE PITCH DEGREES"], c["PLANE BANK DEGREES"],
                      c["PLANE HEADING DEGREES TRUE"], c["PLANE LATITUDE"] * 1.11e5, c["PLANE LONGITUDE"] * 1e5])
        if ref is None or np.any(np.abs(v - ref) > [0.05, 0.03, 0.03, 0.03, 0.03, 0.05, 0.05]):
            ref, t_ref = v, time.time()
        elif time.time() - t_ref >= stable_s and time.time() - t0 >= min_s:
            log(f"  settled after {time.time() - t0:.0f} s (ground {c['GROUND ALTITUDE']:.2f} m)")
            return c
        time.sleep(0.5)
    log("  did not settle")
    return None


def horizon_row(cam, W, H):
    """Picture row of the horizon straight ahead (a ground point 20 km away)."""
    h = math.radians(cam["PLANE HEADING DEGREES TRUE"])
    x, y, ok = T.project(cam, W, H, np.array([[20000 * math.sin(h), 20000 * math.cos(h), cam["GROUND ALTITUDE"]]]))
    return float(y[0]) if ok[0] else H / 2


def cockpit_mask(img, img2, hrow, thresh=3.0):
    """Pixels that did not change after moving the aircraft and are connected to the picture border = cockpit.
    Only below the horizon (+12 px): far away the runway and the sky look the same from both places, and through
    them everything would connect to the border."""
    static = np.abs(img.mean(axis=2) - img2.mean(axis=2)) < thresh
    static[: int(hrow) + 12] = False
    lab, _ = ndimage.label(static)
    border = np.unique(np.concatenate([lab[-1, :], lab[:, 0], lab[:, -1]]))
    mask = np.isin(lab, border[border > 0])
    return ndimage.binary_dilation(mask, iterations=3)


def thin(prof, step=0.1, window=4.0):
    """Keep only narrow bright features (edge and centre stripes, ~1 m) and drop wide ones (touchdown zone and aiming
    point blocks, shoulders): profile minus its running median over `window` metres."""
    p = np.where(np.isfinite(prof), prof, np.nanmedian(prof))
    return np.where(np.isfinite(prof), p - ndimage.median_filter(p, size=int(window / step) | 1, mode="nearest"), np.nan)


def fit_edges(prof, cs, W0, d_range=(-30, 30), w_range=4.0):
    """Symmetric pair of bright stripes in an across profile -> (score, centre, width) or None."""
    base = np.nanmedian(prof)
    best = None
    for w in np.arange(W0 - w_range, W0 + w_range + 1e-9, 0.1):
        for d in np.arange(d_range[0], d_range[1] + 1e-9, 0.05):
            il, ir = np.searchsorted(cs, d - w / 2), np.searchsorted(cs, d + w / 2)
            if il < 3 or ir > len(cs) - 4:
                continue
            s = np.nanmax(prof[il - 3:il + 4]) + np.nanmax(prof[ir - 3:ir + 4]) - 2 * base
            if np.isfinite(s) and (best is None or s > best[0]):
                best = (float(s), float(d), float(w))
    return best


def measure(sim, cp, guid, rw, s, sign, a, log, outdir, tag):
    """Station at `s` m along the facility runway, looking along the primary heading (sign +1) or the reciprocal (-1).
    All results are in the facility frame of the primary heading (cross + = right looking along the primary)."""
    hd = math.radians(rw["HEADING"])
    u, rr = np.array([math.sin(hd), math.cos(hd)]), np.array([math.cos(hd), -math.sin(hd)])
    lat0, lon0, W0 = rw["LATITUDE"], rw["LONGITUDE"], rw["WIDTH"]
    slope = math.tan(math.radians(rw["TRUE_SLOPE"]))
    hdg = (rw["HEADING"] + (0 if sign > 0 else 180)) % 360

    def place(al, cr=0.0):
        e, n = u * al + rr * cr
        lat, lon = C.offset(lat0, lon0, e, n)
        sim.init_pos(lat, lon, 0.0, hdg, True)
        time.sleep(1.0)
        sim.sc.transmit(E_PARK, 1)
        cp.set_view(guid)

    for attempt in range(4):  # a long move can leave the aircraft turned or rolled off the spot while it lands
        place(s)
        c = settle(sim, log)
        if c is None:
            return None
        de, dn = C.enu(lat0, lon0, c["PLANE LATITUDE"], c["PLANE LONGITUDE"])
        off_h = (c["PLANE HEADING DEGREES TRUE"] - hdg + 180) % 360 - 180
        off_c = de * rr[0] + dn * rr[1]
        if abs(off_h) < 0.3 and abs(off_c) < 0.5:
            break
        log(f"  off the spot (heading {off_h:+.2f} deg, cross {off_c:+.2f} m), placing again")
    T.wait_picture(None, log)
    cam = sim.cam()
    img = T.grab()
    place(s + 4.0 * sign, 1.0)
    settle(sim, log, min_s=3.0, stable_s=2.0, timeout=40)
    T.wait_picture(None, log, stable_n=2)
    img2 = T.grab()
    mask = cockpit_mask(img, img2, horizon_row(cam, img.shape[1], img.shape[0]))
    # raw inputs, so the analysis can be redone offline (analyse_saved)
    Image.fromarray(img.astype(np.uint8)).save(os.path.join(outdir, f"{tag}_shot.png"))
    Image.fromarray(img2.astype(np.uint8)).save(os.path.join(outdir, f"{tag}_shot2.png"))
    Image.fromarray((mask * 255).astype(np.uint8)).save(os.path.join(outdir, f"{tag}_mask.png"))
    with open(os.path.join(outdir, f"{tag}_cam.json"), "w", encoding="utf-8") as f:
        json.dump({"cam": cam, "runway": rw, "sign": sign, "near": a.near, "far": a.far}, f, indent=1, default=float)
    return analyse(img, mask, cam, rw, sign, a.near, a.far, log, outdir, tag)


def analyse_saved(outdir, tag, log=print, eye_x_scale=1.0, suffix="", remask=False):
    """Re-run the analysis of a saved station (e.g. with the eye's lateral offset flipped: eye_x_scale=-1).
    remask: compute the cockpit mask again (from _shot2.png, or for older runs from the shot facing the other way)."""
    with open(os.path.join(outdir, f"{tag}_cam.json"), encoding="utf-8") as f:
        d = json.load(f)
    img = np.asarray(Image.open(os.path.join(outdir, f"{tag}_shot.png")), dtype=np.float32)
    cam = dict(d["cam"])
    if remask:
        p2 = os.path.join(outdir, f"{tag}_shot2.png")
        if not os.path.exists(p2):
            p2 = os.path.join(outdir, f"{tag[:-1]}{'r' if tag.endswith('f') else 'f'}_shot.png")
        img2 = np.asarray(Image.open(p2), dtype=np.float32)
        mask = cockpit_mask(img, img2, horizon_row(cam, img.shape[1], img.shape[0]))
    else:
        mask = np.asarray(Image.open(os.path.join(outdir, f"{tag}_mask.png"))) > 127
    cam["L:p42_cp_a_x"] *= eye_x_scale
    return analyse(img, mask, cam, d["runway"], d["sign"], d["near"], d["far"], log, outdir, tag + suffix)


def oa_cross_fn(oa, icao, rw):
    """OurAirports centreline as cross(along) in the facility frame (+ heading difference), or None."""
    p = C.rwy_name(rw["PRIMARY_NUMBER"], rw["PRIMARY_DESIGNATOR"])
    s = C.rwy_name(rw["SECONDARY_NUMBER"], rw["SECONDARY_DESIGNATOR"])
    orow = C.oa_match(oa, icao, p, s)
    line = C.oa_line(orow) if orow else None
    if not line:
        return None
    (la1, lo1), (la2, lo2) = line
    e1, n1 = C.enu(rw["LATITUDE"], rw["LONGITUDE"], la1, lo1)
    e2, n2 = C.enu(rw["LATITUDE"], rw["LONGITUDE"], la2, lo2)
    hh = math.radians(rw["HEADING"])
    a1, c1 = e1 * math.sin(hh) + n1 * math.cos(hh), e1 * math.cos(hh) - n1 * math.sin(hh)
    a2, c2 = e2 * math.sin(hh) + n2 * math.cos(hh), e2 * math.cos(hh) - n2 * math.sin(hh)
    if a2 < a1:  # OurAirports "le" may be either end
        a1, c1, a2, c2 = a2, c2, a1, c1
    return (lambda al: c1 + (al - a1) / (a2 - a1) * (c2 - c1)), math.degrees(math.atan2(c2 - c1, a2 - a1))


def summarise(outdir, oa_path, remask=True, eye_x_scale=1.0):
    """Offline: analyse every saved station of a run again and print/save the comparison per runway."""
    import glob
    import re
    oa = C.oa_load(oa_path)
    rows = {}
    for p in sorted(glob.glob(os.path.join(outdir, "*_cam.json"))):
        tag = os.path.basename(p)[:-9]
        m = re.match(r"(\w+?)_(\w+)_(\d)([fr])$", tag)
        if not m:
            continue
        r = analyse_saved(outdir, tag, log=lambda s: None, eye_x_scale=eye_x_scale, remask=remask, suffix="_sum")
        with open(p, encoding="utf-8") as f:
            rw = json.load(f)["runway"]
        rows.setdefault((m[1], m[2]), {"rw": rw, "st": {}})["st"].setdefault(int(m[3]), {})[m[4]] = r
    out = []
    for (icao, rname), d in rows.items():
        rw = d["rw"]
        oaf = oa_cross_fn(oa, icao, rw)
        for k, pair in sorted(d["st"].items()):
            f_, r_ = pair.get("f"), pair.get("r")
            ok = [x for x in (f_, r_) if x and x["quality"] >= MIN_QUALITY]
            row = {"icao": icao, "runway": rname, "station": k, "n": len(ok), "facility_hdg": rw["HEADING"],
                   "width_facility": rw["WIDTH"]}
            if len(ok) == 2:
                row.update(offset=(f_["centre_cross"] + r_["centre_cross"]) / 2,
                           half_diff=(f_["centre_cross"] - r_["centre_cross"]) / 2,
                           width=(f_["width"] + r_["width"]) / 2, along=f_["aircraft_along"])
            elif len(ok) == 1:  # one direction only: offset still carries the eye error (~1 m)
                row.update(offset=ok[0]["centre_cross"], width=ok[0]["width"], along=ok[0]["aircraft_along"])
            if oaf and "along" in row:
                row["ourairports"] = oaf[0](row["along"])
                row["oa_heading_diff"] = oaf[1]
            out.append(row)
    with open(os.path.join(outdir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, default=float)
    print(f"{'runway':12} {'st':>2} {'n':>1} {'along':>7} {'picture':>8} {'OA':>8} {'half':>6} {'width':>9}")
    for r in out:
        def fm(k, w=8):
            return f"{r[k]:+{w}.2f}" if k in r else " " * (w - 1) + "-"
        print(f"{r['icao'] + ' ' + r['runway']:12} {r['station']:>2} {r['n']:>1} {fm('along', 7)} {fm('offset')} "
              f"{fm('ourairports')} {fm('half_diff', 6)} {r.get('width', float('nan')):5.1f}/{r['width_facility']:.0f}")
    return out


def analyse(img, mask, cam, rw, sign, near, far, log, outdir, tag):
    hd = math.radians(rw["HEADING"])
    u, rr = np.array([math.sin(hd), math.cos(hd)]), np.array([math.cos(hd), -math.sin(hd)])
    lat0, lon0, W0 = rw["LATITUDE"], rw["LONGITUDE"], rw["WIDTH"]
    slope = math.tan(math.radians(rw["TRUE_SLOPE"]))
    H, W = img.shape[:2]

    # aircraft along/cross in the facility frame (it may have shifted a little while settling)
    de, dn = C.enu(lat0, lon0, cam["PLANE LATITUDE"], cam["PLANE LONGITUDE"])
    ac_along, ac_cross = de * u[0] + dn * u[1], de * rr[0] + dn * rr[1]
    half_c = W0 / 2 + 15
    cs = np.arange(-half_c, half_c + 1e-9, 0.1)
    als = ac_along + sign * np.arange(near, far + 1e-9, 0.5)  # rows: near -> far
    A, Cc = np.meshgrid(als, cs, indexing="ij")
    e = (u[0] * A + rr[0] * Cc).ravel() - de
    n = (u[1] * A + rr[1] * Cc).ravel() - dn
    z = cam["GROUND ALTITUDE"] + (A.ravel() - ac_along) * slope
    x, y, ok = T.project(cam, W, H, np.column_stack([e, n, z]))
    ok[ok] &= ~mask[y[ok].astype(int), x[ok].astype(int)]
    vals = np.full((e.size, 3), np.nan, dtype=np.float32)
    vals[ok] = T.bilinear(img, x[ok], y[ok])
    ortho = vals.reshape(len(als), len(cs), 3)
    lum = np.nanmean(ortho, axis=2)
    rows_ok = np.isfinite(lum).mean(axis=1) > 0.6
    log(f"  {tag}: aircraft at along {ac_along:+.1f}, cross {ac_cross:+.2f} m, hdg {cam['PLANE HEADING DEGREES TRUE']:.2f}; "
        f"{rows_ok.sum()} of {len(als)} ortho rows usable")
    if rows_ok.sum() < 40:
        return None
    L = lum[rows_ok]
    p90 = thin(np.nanpercentile(L, 90, axis=0))
    best = fit_edges(p90, cs, W0)
    # quality: the weaker of the two edge peaks against the profile's noise (no painted edge line -> weak)
    quality = None
    if best:
        mad = np.nanmedian(np.abs(p90 - np.nanmedian(p90))) + 1e-3
        pk = [np.nanmax(p90[max(i - 3, 0):i + 4]) for i in (np.searchsorted(cs, best[1] - best[2] / 2),
                                                           np.searchsorted(cs, best[1] + best[2] / 2))]
        quality = float(min(pk) / mad)
    # centreline stripe: brightest narrow column near the fitted centre
    stripe = None
    if best:
        sel = np.abs(cs - best[1]) < 6
        k = np.nanargmax(np.where(sel, np.nan_to_num(p90, nan=-1e9), -np.inf))
        stripe = float(cs[k])
    # heading: near half vs far half of the usable rows, width fixed
    halves = []
    if best:
        idx = np.where(rows_ok)[0]
        for part in (idx[: len(idx) // 2], idx[len(idx) // 2:]):
            q = thin(np.nanpercentile(lum[part], 90, axis=0))
            hb = fit_edges(q, cs, best[2], (best[1] - 4, best[1] + 4), 0.0)
            halves.append((float(np.mean(als[part])), hb[1] if hb else None))
    dh = None
    if len(halves) == 2 and all(d is not None for _, d in halves):
        dh = math.degrees(math.atan((halves[1][1] - halves[0][1]) / (halves[1][0] - halves[0][0])))

    # --- pictures to check by eye
    def to_px(al, cr):
        ee, nn = u * al + rr * cr
        zz = cam["GROUND ALTITUDE"] + (al - ac_along) * slope
        xx, yy, okk = T.project(cam, W, H, np.array([[ee - de, nn - dn, zz]]))
        return (float(xx[0]), float(yy[0])) if okk[0] else None

    ov = Image.fromarray(img.astype(np.uint8))
    dr = ImageDraw.Draw(ov)
    alongs = ac_along + sign * np.linspace(near, far, 40)

    def polyline(cr, col):
        pts = [p for p in (to_px(al, cr) for al in alongs) if p]
        if len(pts) > 1:
            dr.line(pts, fill=col, width=1)

    for cr in (-W0 / 2, W0 / 2):
        polyline(cr, (255, 60, 60))
    if best:
        for cr in (best[1] - best[2] / 2, best[1] + best[2] / 2):
            polyline(cr, (60, 255, 60))
    ov.save(os.path.join(outdir, f"{tag}_overlay.png"))
    vis = np.nan_to_num(ortho[::-1], nan=40).clip(0, 255).astype(np.uint8)  # far at the top; columns: facility cross
    im = Image.fromarray(vis).resize((vis.shape[1] * 2, vis.shape[0] * 2), Image.NEAREST)
    d2 = ImageDraw.Draw(im)

    def col(c):
        return int((c + half_c) / 0.1 * 2)

    for c in (-W0 / 2, W0 / 2):
        d2.line([(col(c), 0), (col(c), im.height)], fill=(255, 60, 60))
    if best:
        for c in (best[1] - best[2] / 2, best[1] + best[2] / 2):
            d2.line([(col(c), 0), (col(c), im.height)], fill=(60, 255, 60))
    im.save(os.path.join(outdir, f"{tag}_ortho.png"))
    pw, ph = 900, 300
    pim = Image.new("RGB", (pw, ph), (20, 20, 20))
    pd = ImageDraw.Draw(pim)
    pd.line([(i / len(cs) * pw, ph - 10 - (0 if not np.isfinite(v) else v) / 255 * (ph - 20)) for i, v in enumerate(p90)],
            fill=(255, 255, 255))
    for c, colr in [(-W0 / 2, (255, 60, 60)), (W0 / 2, (255, 60, 60))] + \
            ([(best[1] - best[2] / 2, (60, 255, 60)), (best[1] + best[2] / 2, (60, 255, 60))] if best else []):
        xx = (c + half_c) / (2 * half_c) * pw
        pd.line([(xx, 0), (xx, ph)], fill=colr)
    pim.save(os.path.join(outdir, f"{tag}_profile.png"))
    if not best:
        return None
    return {"along": float(ac_along + sign * (near + far) / 2), "sign": sign, "aircraft_along": float(ac_along),
            "aircraft_cross": float(ac_cross), "centre_cross": best[1], "width": best[2], "stripe_cross": stripe,
            "heading_diff": dh, "halves": halves, "score": best[0], "quality": quality, "zoom": cam["L:p42_cp_r_zoom"],
            "client": [W, H]}


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--summarise":  # offline: forward.py --summarise <run folder> [--eye-flip]
        summarise(sys.argv[2], os.path.join(HERE, "data", "runways.csv"),
                  eye_x_scale=-1.0 if "--eye-flip" in sys.argv else 1.0)
        return
    ap = argparse.ArgumentParser()
    ap.add_argument("icaos", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--near", type=float, default=25.0, help="rectify from this far ahead of the aircraft, m")
    ap.add_argument("--far", type=float, default=220.0)
    ap.add_argument("--stations", default="-0.2,0,0.2", help="along positions as fractions of the length "
                                                               "(clear of the touchdown zone markings)")
    ap.add_argument("--min-length", type=float, default=1200)
    ap.add_argument("--daylight", action="store_true", help="local solar noon per airport, clock put back at the end")
    ap.add_argument("--dll", default=os.path.join(HERE, "..", "..", "build", "release", "SimConnect.dll"))
    ap.add_argument("--oa", default=os.path.join(HERE, "data", "runways.csv"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(a.out, "forward.log"), "a", encoding="utf-8")

    def log(s):
        print(s, flush=True)
        logf.write(s + "\n")
        logf.flush()

    h = T.msfs_hwnd()
    if not h:
        sys.exit("MSFS window not found")
    T.GRABBER["g"] = T.WindowGrabber(h)
    if CpRemote is None:
        sys.exit("needs cpremote.py, a client for the ChasePlane Remote WebSocket (not included: undocumented protocol)")
    cp = CpRemote()
    if cp.mode != 0:
        sys.exit(f"put ChasePlane in a cockpit view first (mode {cp.mode})")
    guid = cp.preset_guid
    oa = C.oa_load(a.oa)
    sim = FwdSim(os.path.abspath(a.dll))
    home = sim.read()
    brake0 = sim.brake()
    zulu0, t0 = sim.zulu_s(), time.time()
    log(f"start {home['PLANE LATITUDE']:.6f},{home['PLANE LONGITUDE']:.6f} client {T.client_rect(h)} view {guid} "
        f"zulu {zulu0 / 3600:.2f} h brake {brake0:.0f}")
    if home["IS SLEW ACTIVE"] >= 0.5:
        sim.sc.transmit(C.E_SLEW_OFF)
    sim.sc.transmit(C.E_PAUSE_OFF)
    results = []
    try:
        for icao in a.icaos:
            rws = [r for r in sim.runways(icao) if r["LENGTH"] >= a.min_length]
            if a.daylight and rws:
                noon = (12.0 - rws[0]["LONGITUDE"] / 15.0) % 24.0
                sim.set_zulu(noon * 3600)
                log(f"{icao}: clock set to local solar noon ({noon:.1f} Z)")
            for rw in rws:
                p, s_ = C.rwy_name(rw["PRIMARY_NUMBER"], rw["PRIMARY_DESIGNATOR"]), \
                    C.rwy_name(rw["SECONDARY_NUMBER"], rw["SECONDARY_DESIGNATOR"])
                name = f"{p}/{s_}"
                log(f"{icao} {name}: facility {rw['LATITUDE']:.6f},{rw['LONGITUDE']:.6f} hdg {rw['HEADING']:.2f} "
                    f"{rw['LENGTH']:.0f}x{rw['WIDTH']:.0f} slope {rw['TRUE_SLOPE']:+.2f}")
                cuts = []
                for k, frac in enumerate(float(x) for x in a.stations.split(",")):
                  for sign in (1, -1):
                    tag = f"{icao}_{p}_{k}{'f' if sign > 0 else 'r'}"
                    r = measure(sim, cp, guid, rw, frac * rw["LENGTH"], sign, a, log, a.out, tag)
                    if r:
                        r["station"] = k
                        log(f"   {'->' if sign > 0 else '<-'} along {r['along']:+6.0f}: centre {r['centre_cross']:+.2f} m (stripe "
                            f"{r['stripe_cross'] if r['stripe_cross'] is None else round(r['stripe_cross'], 2)}), "
                            f"width {r['width']:.1f} m, heading diff "
                            f"{'?' if r['heading_diff'] is None else f'{r['heading_diff']:+.3f}'} deg, "
                            f"quality {r['quality']:.0f}{'' if r['quality'] >= MIN_QUALITY else ' (weak, not used)'}")
                        cuts.append(r)
                # both directions at one station: an error in the eye's lateral offset (or a camera yaw) moves the
                # picture runway to opposite sides in the facility frame, a real offset to the same side
                # -> mean = offset, half difference = eye/yaw error
                stations = []
                for k in sorted({c["station"] for c in cuts}):
                    pair = [c for c in cuts if c["station"] == k and c["quality"] >= MIN_QUALITY]
                    if len(pair) == 2:
                        f_, r_ = sorted(pair, key=lambda c: -c["sign"])
                        stations.append({"along": f_["aircraft_along"],
                                         "offset": (f_["centre_cross"] + r_["centre_cross"]) / 2,
                                         "width": (f_["width"] + r_["width"]) / 2,
                                         "half_diff": (f_["centre_cross"] - r_["centre_cross"]) / 2})
                res = {"icao": icao, "runway": name, "facility": rw, "cuts": cuts, "stations": stations}
                orow = C.oa_match(oa, icao, p, s_)
                line = C.oa_line(orow) if orow else None
                if line:
                    (la1, lo1), (la2, lo2) = line
                    e1, n1 = C.enu(rw["LATITUDE"], rw["LONGITUDE"], la1, lo1)
                    e2, n2 = C.enu(rw["LATITUDE"], rw["LONGITUDE"], la2, lo2)
                    hh = math.radians(rw["HEADING"])
                    a1, c1 = e1 * math.sin(hh) + n1 * math.cos(hh), e1 * math.cos(hh) - n1 * math.sin(hh)
                    a2, c2 = e2 * math.sin(hh) + n2 * math.cos(hh), e2 * math.cos(hh) - n2 * math.sin(hh)
                    if a2 < a1:  # OurAirports "le" may be either end
                        a1, c1, a2, c2 = a2, c2, a1, c1
                    res["ourairports_heading_diff"] = math.degrees(math.atan2(c2 - c1, a2 - a1))
                    for st in stations:
                        st["ourairports"] = c1 + (st["along"] - a1) / (a2 - a1) * (c2 - c1)
                for st in stations:
                    oa_s = f", OurAirports {st['ourairports']:+.2f} m" if "ourairports" in st else ""
                    log(f"   station {st['along']:+6.0f}: picture centre {st['offset']:+.2f} m from the facility "
                        f"centreline{oa_s}; width {st['width']:.1f} m; half difference {st['half_diff']:+.2f} m")
                if "ourairports_heading_diff" in res:
                    log(f"   OurAirports heading vs facility {res['ourairports_heading_diff']:+.3f} deg")
                results.append(res)
                with open(os.path.join(a.out, "results.json"), "w", encoding="utf-8") as f:
                    json.dump(results, f, indent=1, default=float)
    finally:
        # back on the ground where it was. After a long move the scenery streams in while the aircraft is placed and
        # it can be left falling from tens of metres (11/15: paused 28 m above the stand) -> wait until it is down
        # and steady, place again if not, and only then pause
        h0 = home["PLANE ALTITUDE"] - home["GROUND ALTITUDE"]
        for _ in range(4):
            sim.init_pos(home["PLANE LATITUDE"], home["PLANE LONGITUDE"], 0.0, home["PLANE HEADING DEGREES TRUE"], True)
            time.sleep(1.0)
            sim.sc.transmit(E_PARK, 1)
            c = settle(sim, log, min_s=8.0, stable_s=4.0, timeout=90)
            if c and abs((c["PLANE ALTITUDE"] - c["GROUND ALTITUDE"]) - h0) < 1.0 and abs(c["PLANE PITCH DEGREES"]) < 3:
                break
            log("  not down on the stand yet, placing again")
        sim.sc.transmit(E_PARK, int(brake0 >= 0.5))
        cp.set_view(guid)
        if a.daylight:
            sim.set_zulu(zulu0 + (time.time() - t0))
            log("clock put back")
        time.sleep(2.0)
        sim.sc.transmit(C.E_PAUSE_ON)
        log("aircraft put back, sim paused")
        sim.sc.close()
        cp.close()
        T.GRABBER["g"].stop()


if __name__ == "__main__":
    main()
