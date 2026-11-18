"""Runway data experiment: where is the runway the sim actually draws, compared with the data sources?

For each runway of the given airports the user aircraft is slewed onto the runway and moved sideways across it in
2 m steps (edges then refined to ~0.1 m) at three places along it. The band of the same `SURFACE TYPE` as at the
facility centreline (asphalt/concrete; paved shoulders included, so the width can exceed the runway's) gives the
pavement edges and its midpoint the real centreline; `GROUND ALTITUDE` gives the surface height. Compared with:
  - the MSFS facility data (what Birdie uses now: centre, heading, ALTITUDE, TRUE_SLOPE),
  - OurAirports runway ends (public domain, https://ourairports.com/data/).

Findings that shaped this (11/15, ZSHC): while the sim is paused GROUND ALTITUDE / SURFACE TYPE do not follow a
moved aircraft, so the run unpauses (slew keeps the aircraft still) and pauses again at the end; ON ANY RUNWAY stays 1
even on the grass, so it is not used. Scenery streams in: after each long move the run waits until the ground altitude
has been steady for several seconds, and each cut is checked again at its end.

MOVES THE USER AIRCRAFT (slew mode) - run only when not flying. The aircraft is put back where it was at the end.

    python -I tools/runway_calib/collect.py ZSNB ZSHC KASE --out <folder>
"""
import argparse
import csv
import ctypes
import json
import math
import os
import struct
import sys
import time
import urllib.request
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "reference", "python"))
from simconnect_min import SimConnect  # noqa: E402

R_EARTH = 6378137.0
OA_URL = "https://davidmegginson.github.io/ourairports-data/runways.csv"
DESIG = {0: "", 1: "L", 2: "R", 3: "C", 4: "W", 5: "A", 6: "B"}

# data definitions / requests
D_POS, D_READ, D_RWY = 1, 2, 77
E_SLEW_ON, E_SLEW_OFF, E_PAUSE_ON, E_PAUSE_OFF = 1, 2, 3, 4
PROBE_WAIT = 0.25  # s after moving before reading the surface (slew + unpaused: updates within a frame or two)
READ_VARS = [("PLANE LATITUDE", "degrees"), ("PLANE LONGITUDE", "degrees"), ("PLANE ALTITUDE", "meters"),
             ("GROUND ALTITUDE", "meters"), ("ON ANY RUNWAY", "bool"), ("SURFACE TYPE", "enum"),
             ("PLANE HEADING DEGREES TRUE", "degrees"), ("IS SLEW ACTIVE", "bool")]
POS_VARS = [("PLANE LATITUDE", "degrees"), ("PLANE LONGITUDE", "degrees"), ("PLANE ALTITUDE", "meters"),
            ("PLANE HEADING DEGREES TRUE", "degrees"), ("PLANE PITCH DEGREES", "degrees"),
            ("PLANE BANK DEGREES", "degrees")]
RWY_FIELDS = ["LATITUDE", "LONGITUDE", "ALTITUDE", "HEADING", "LENGTH", "WIDTH", "PRIMARY_NUMBER",
              "PRIMARY_DESIGNATOR", "SECONDARY_NUMBER", "SECONDARY_DESIGNATOR", "TRUE_SLOPE"]
RWY_FMT = "<dddfffiiiif"


E2 = 6.69437999014e-3  # WGS84 first eccentricity squared
ELLIPSOID = True  # False: the old sphere of radius R_EARTH (north 0.3 % too long, east 0.1 % too short at 35 deg)


def radii(lat):
    """Local metres per radian: (north = meridional radius M, east = prime vertical N * cos(lat))."""
    if not ELLIPSOID:
        return R_EARTH, R_EARTH * math.cos(math.radians(lat))
    s2 = math.sin(math.radians(lat)) ** 2
    w = 1.0 - E2 * s2
    return R_EARTH * (1.0 - E2) / w ** 1.5, R_EARTH / math.sqrt(w) * math.cos(math.radians(lat))


def offset(lat, lon, e, n):
    rn, re = radii(lat)
    return lat + math.degrees(n / rn), lon + math.degrees(e / re)


def enu(lat0, lon0, lat, lon):
    rn, re = radii(lat0)
    return math.radians(lon - lon0) * re, math.radians(lat - lat0) * rn


def rwy_name(num, des):
    return f"{num:02d}{DESIG.get(des, '')}" if 1 <= num <= 36 else f"#{num}"


def norm_ident(s):
    s = s.strip().upper()
    i = 0
    while i < len(s) - 1 and s[i] == "0" and s[i + 1].isdigit():
        i += 1
    return s[i:]


class Sim:
    def __init__(self, dll):
        self.sc = SimConnect("birdie_runway_calib", dll)
        d = self.sc.dll
        d.SimConnect_AddToFacilityDefinition.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_char_p]
        d.SimConnect_RequestFacilityData.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, ctypes.c_char_p,
                                                     ctypes.c_char_p]
        for n, u in READ_VARS:
            self.sc.add_var(D_READ, n, u)
        for n, u in POS_VARS:
            self.sc.add_var(D_POS, n, u)
        for f in ["OPEN AIRPORT", "OPEN RUNWAY"] + RWY_FIELDS + ["CLOSE RUNWAY", "CLOSE AIRPORT"]:
            d.SimConnect_AddToFacilityDefinition(self.sc.h, D_RWY, f.encode())
        self.sc.map_event(E_SLEW_ON, "SLEW_ON")
        self.sc.map_event(E_SLEW_OFF, "SLEW_OFF")
        self.sc.map_event(E_PAUSE_ON, "PAUSE_ON")
        self.sc.map_event(E_PAUSE_OFF, "PAUSE_OFF")
        self.req = 100

    def read(self, timeout=2.0):
        self.req += 1
        rid = self.req
        self.sc.request(rid, D_READ, 1)
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.sc.poll():
                if m[0] == "data" and m[1] == rid:
                    return dict(zip([n for n, _ in READ_VARS], struct.unpack_from(f"<{len(READ_VARS)}d", m[3])))
            time.sleep(0.005)
        raise TimeoutError("no data from the sim")

    def runways(self, icao, timeout=5.0):
        self.req += 1
        rid = self.req
        self.sc.dll.SimConnect_RequestFacilityData(self.sc.h, D_RWY, rid, icao.encode(), b"")
        out, t0 = [], time.time()
        # raw dispatch (simconnect_min.poll drops facility messages): facility data = recv id 28, end = 29
        ptr, size = ctypes.c_void_p(), wintypes.DWORD()
        while time.time() - t0 < timeout:
            while self.sc.dll.SimConnect_GetNextDispatch(self.sc.h, ctypes.byref(ptr), ctypes.byref(size)) == 0:
                raw = ctypes.string_at(ptr.value, size.value)
                _, _, recv = struct.unpack_from("<III", raw, 0)
                if recv == 28:
                    req, _u, _p, typ = struct.unpack_from("<4I", raw, 12)
                    if req == rid and typ == 1:
                        out.append(dict(zip(RWY_FIELDS, struct.unpack_from(RWY_FMT, raw, 40))))
                elif recv == 29 and struct.unpack_from("<I", raw, 12)[0] == rid:
                    return out
            time.sleep(0.01)
        return out

    def place(self, lat, lon, alt, hdg):
        self.sc.set_data(D_POS, [lat, lon, alt, hdg, 0.0, 0.0])

    def settle(self, lat, lon, frames=3, timeout=2.0):
        """Wait until the sim reports the aircraft at (lat, lon) for a few reads."""
        ok, t0, last = 0, time.time(), None
        while time.time() - t0 < timeout:
            r = self.read()
            e, n = enu(lat, lon, r["PLANE LATITUDE"], r["PLANE LONGITUDE"])
            if math.hypot(e, n) < 0.05:
                ok += 1
                last = r
                if ok >= frames:
                    return last
            else:
                ok = 0
        return last


def wait_scenery(sim, lat, lon, hdg, log, stable_s=5.0, min_s=8.0, timeout=120.0):
    """After a long move: scenery streams in. Wait at least min_s, then until the ground altitude under the aircraft
    has stayed within 5 cm for stable_s."""
    t0, hist = time.time(), []
    while time.time() - t0 < timeout:
        r = sim.read()
        sim.place(lat, lon, r["GROUND ALTITUDE"] + 1.5, hdg)
        hist.append((time.time(), r["GROUND ALTITUDE"]))
        hist = [x for x in hist if x[0] > time.time() - stable_s]
        if time.time() - t0 > max(min_s, stable_s) and max(x[1] for x in hist) - min(x[1] for x in hist) < 0.05:
            log(f"  scenery steady after {time.time() - t0:.0f} s (ground {r['GROUND ALTITUDE']:.2f} m)")
            return r["GROUND ALTITUDE"]
        time.sleep(0.25)
    log("  scenery did not settle in time")
    return None


def surface_at(sim, lat, lon, hdg):
    """Move the (slewed) aircraft just above the ground at (lat, lon); returns (surface type, ground altitude)."""
    r = sim.read()
    sim.place(lat, lon, r["GROUND ALTITUDE"] + 1.5, hdg)
    time.sleep(PROBE_WAIT)
    r = sim.settle(lat, lon, frames=2) or sim.read()
    return int(r["SURFACE TYPE"]), r["GROUND ALTITUDE"]


def sweep(sim, rw, along, half_span, step=2.0):
    """Pavement edges across the facility centreline at `along` m from the facility centre: the band of the same
    surface type as on the centreline."""
    h = math.radians(rw["HEADING"])
    u, rr = (math.sin(h), math.cos(h)), (math.cos(h), -math.sin(h))

    def probe(c):
        lat, lon = offset(rw["LATITUDE"], rw["LONGITUDE"], u[0] * along + rr[0] * c, u[1] * along + rr[1] * c)
        return surface_at(sim, lat, lon, rw["HEADING"])

    surf0, gnd0 = probe(0.0)
    n = int(half_span / step)
    same = {i: probe(i * step)[0] == surf0 for i in range(-n, n + 1)}
    if not same[0]:
        return None
    lo = 0
    while lo - 1 >= -n and same[lo - 1]:
        lo -= 1
    hi = 0
    while hi + 1 <= n and same[hi + 1]:
        hi += 1

    def refine(inside, outside):
        for _ in range(5):  # 2 m -> 6 cm
            mid = (inside + outside) / 2
            if probe(mid)[0] == surf0:
                inside = mid
            else:
                outside = mid
        return (inside + outside) / 2

    left = refine(lo * step, (lo - 1) * step) if lo > -n else -half_span
    right = refine(hi * step, (hi + 1) * step) if hi < n else half_span
    centre = (left + right) / 2
    _, gnd = probe(centre)
    _, gnd_again = probe(0.0)  # streaming check: the ground must not have changed during the cut
    return {"along": along, "left": left, "right": right, "centre_cross": centre, "width": right - left,
            "ground_alt": gnd, "surface_type": surf0, "edge_hit_span": lo == -n or hi == n,
            "ground_drift_m": gnd_again - gnd0}


def oa_load(path):
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        urllib.request.urlretrieve(OA_URL, path)
    rows = {}
    with open(path, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            rows.setdefault(r["airport_ident"], []).append(r)
    return rows


def oa_match(oa_rows, icao, prim, sec):
    for r in oa_rows.get(icao, []):
        a, b = norm_ident(r["le_ident"]), norm_ident(r["he_ident"])
        if {a, b} == {norm_ident(prim), norm_ident(sec)}:
            return r
    return None


def oa_line(r):
    try:
        la1, lo1, la2, lo2 = (float(r[k]) for k in ("le_latitude_deg", "le_longitude_deg", "he_latitude_deg",
                                                    "he_longitude_deg"))
    except (TypeError, ValueError):
        return None
    return (la1, lo1), (la2, lo2)


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("icaos", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--dll", default=os.path.join(HERE, "..", "..", "build", "release", "SimConnect.dll"))
    ap.add_argument("--oa", default=os.path.join(HERE, "data", "runways.csv"))
    ap.add_argument("--min-length", type=float, default=1200)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(a.out, "collect.log"), "a", encoding="utf-8")

    def log(s):
        print(s, flush=True)
        logf.write(s + "\n")
        logf.flush()

    oa = oa_load(a.oa)
    sim = Sim(os.path.abspath(a.dll))
    home = sim.read()
    log(f"start at {home['PLANE LATITUDE']:.6f},{home['PLANE LONGITUDE']:.6f} alt {home['PLANE ALTITUDE']:.1f} m")
    sim.sc.transmit(E_SLEW_ON)
    time.sleep(0.3)
    sim.sc.transmit(E_PAUSE_OFF)  # surface data only updates while the sim runs; slew keeps the aircraft still
    time.sleep(0.5)
    results = []
    try:
        for icao in a.icaos:
            rws = sim.runways(icao)
            log(f"{icao}: {len(rws)} runway records")
            for rw in rws:
                if rw["LENGTH"] < a.min_length:
                    continue
                name = f"{rwy_name(rw['PRIMARY_NUMBER'], rw['PRIMARY_DESIGNATOR'])}/" \
                       f"{rwy_name(rw['SECONDARY_NUMBER'], rw['SECONDARY_DESIGNATOR'])}"
                log(f"{icao} {name}: facility centre {rw['LATITUDE']:.6f},{rw['LONGITUDE']:.6f} hdg {rw['HEADING']:.2f}"
                    f" {rw['LENGTH']:.0f}x{rw['WIDTH']:.0f} alt {rw['ALTITUDE']:.2f} slope {rw['TRUE_SLOPE']:+.3f}")
                h = math.radians(rw["HEADING"])
                lat0, lon0 = rw["LATITUDE"], rw["LONGITUDE"]
                sim.place(lat0, lon0, rw["ALTITUDE"] + 50, rw["HEADING"])
                if wait_scenery(sim, lat0, lon0, rw["HEADING"], log) is None:
                    continue
                cuts = []
                for frac in (-0.4, 0.0, 0.4):
                    s = sweep(sim, rw, frac * rw["LENGTH"], rw["WIDTH"] / 2 + 40)
                    if s and abs(s["ground_drift_m"]) > 0.05:  # scenery still streaming: settle and redo once
                        log(f"   along {s['along']:+7.0f} m: ground moved {s['ground_drift_m']:+.2f} m during the cut, redo")
                        wait_scenery(sim, lat0, lon0, rw["HEADING"], log)
                        s = sweep(sim, rw, frac * rw["LENGTH"], rw["WIDTH"] / 2 + 40)
                    if s:
                        log(f"   along {s['along']:+7.0f} m: pavement {s['left']:+6.2f} .. {s['right']:+6.2f} m "
                            f"(width {s['width']:.1f}) centre {s['centre_cross']:+.2f} m, ground {s['ground_alt']:.2f} m"
                            f", surface {s['surface_type']}{'  (edge outside sweep!)' if s['edge_hit_span'] else ''}"
                            f"{'  (ground drift %+.2f m)' % s['ground_drift_m'] if abs(s['ground_drift_m']) > 0.05 else ''}")
                        cuts.append(s)
                    else:
                        log(f"   along {frac * rw['LENGTH']:+7.0f} m: no runway found across the sweep")
                res = {"icao": icao, "runway": name, "facility": rw, "cuts": cuts}
                # measured centreline vs facility: offset at the centre and heading difference
                if len(cuts) >= 2:
                    xs = [c["along"] for c in cuts]
                    ys = [c["centre_cross"] for c in cuts]
                    n = len(xs)
                    mx, my = sum(xs) / n, sum(ys) / n
                    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
                    res["measured_vs_facility"] = {"cross_at_centre_m": my - slope * mx,
                                                   "heading_diff_deg": math.degrees(math.atan(slope))}
                    zs = [c["ground_alt"] for c in cuts]
                    mz = sum(zs) / n
                    zslope = sum((x - mx) * (z - mz) for x, z in zip(xs, zs)) / sum((x - mx) ** 2 for x in xs)
                    res["measured_surface"] = {"alt_at_facility_centre_m": mz - zslope * mx,
                                               "slope_deg": math.degrees(math.atan(zslope))}
                    log(f"   measured vs facility: centreline {res['measured_vs_facility']['cross_at_centre_m']:+.2f} m"
                        f" right, heading {res['measured_vs_facility']['heading_diff_deg']:+.3f} deg; surface at centre "
                        f"{res['measured_surface']['alt_at_facility_centre_m']:.2f} m (facility ALTITUDE "
                        f"{rw['ALTITUDE']:.2f}), slope {res['measured_surface']['slope_deg']:+.3f} (TRUE_SLOPE "
                        f"{rw['TRUE_SLOPE']:+.3f})")
                # OurAirports
                r = oa_match(oa, icao, rwy_name(rw["PRIMARY_NUMBER"], rw["PRIMARY_DESIGNATOR"]),
                             rwy_name(rw["SECONDARY_NUMBER"], rw["SECONDARY_DESIGNATOR"]))
                line = oa_line(r) if r else None
                if line and cuts:
                    (la1, lo1), (la2, lo2) = line
                    pts = []
                    for c in cuts:  # OurAirports centreline cross offset at each cut, relative to the facility line
                        e1, n1 = enu(lat0, lon0, la1, lo1)
                        e2, n2 = enu(lat0, lon0, la2, lo2)
                        a1 = e1 * math.sin(h) + n1 * math.cos(h)
                        c1 = e1 * math.cos(h) - n1 * math.sin(h)
                        a2 = e2 * math.sin(h) + n2 * math.cos(h)
                        c2 = e2 * math.cos(h) - n2 * math.sin(h)
                        t = (c["along"] - a1) / (a2 - a1)
                        pts.append(c1 + t * (c2 - c1))
                    res["ourairports"] = {"cross_at_cuts_m": pts,
                                          "minus_measured_m": [p - c["centre_cross"] for p, c in zip(pts, cuts)],
                                          "le_elev_ft": fnum(r.get("le_elevation_ft")),
                                          "he_elev_ft": fnum(r.get("he_elevation_ft"))}
                    log("   OurAirports centreline minus measured: " +
                        ", ".join(f"{d:+.2f}" for d in res["ourairports"]["minus_measured_m"]) +
                        "  | facility minus measured: " + ", ".join(f"{-c['centre_cross']:+.2f}" for c in cuts))
                elif not r:
                    log("   OurAirports: no matching runway")
                results.append(res)
                with open(os.path.join(a.out, "results.json"), "w", encoding="utf-8") as f:
                    json.dump(results, f, indent=1)
    finally:
        for _ in range(8):  # a few times: the home scenery may still be streaming back in
            sim.place(home["PLANE LATITUDE"], home["PLANE LONGITUDE"], home["PLANE ALTITUDE"],
                      home["PLANE HEADING DEGREES TRUE"])
            time.sleep(0.5)
        sim.sc.transmit(E_PAUSE_ON)  # leave it paused (a run starts from a paused, parked aircraft)
        time.sleep(0.3)
        if home["IS SLEW ACTIVE"] < 0.5:
            sim.sc.transmit(E_SLEW_OFF)
        log("aircraft put back, sim paused")
        sim.sc.close()


if __name__ == "__main__":
    main()
