"""Runway data from MSFS (SimConnect facilities API), runway selection and screen projection.

Facility layout (MSFS 2024 SDK SimConnect.h, pack 1):
  RECV_ID_AIRPORT_LIST (18): RECV(12) + RequestID, ArraySize, EntryNumber, OutOf (16) + ArraySize x
                             {Ident char[9], Region char[3], lat f64, lon f64, alt_m f64} (36 bytes)
  RECV_ID_FACILITY_DATA (28): RECV(12) + UserRequestId, UniqueRequestId, ParentUniqueRequestId, Type, IsListItem,
                             ItemIndex, ListSize (28) + data in definition order
  RECV_ID_FACILITY_DATA_END (29): RECV(12) + RequestId
Runway fields: LATITUDE/LONGITUDE/ALTITUDE f64 (centre, metres), HEADING/LENGTH/WIDTH f32 (true deg, metres),
PRIMARY/SECONDARY_NUMBER and _DESIGNATOR i32, TRUE_SLOPE f32 (deg along the primary direction, + = uphill;
see SLOPE_SIGN).
"""
import ctypes
import math
import struct
from ctypes import wintypes

import numpy as np

R_EARTH = 6378137.0
RECV_AIRPORT_LIST, RECV_FACILITY_DATA, RECV_FACILITY_DATA_END = 18, 28, 29
FAC_AIRPORT, FAC_RUNWAY = 0, 1
DESIG = {0: "", 1: "L", 2: "R", 3: "C", 4: "W", 5: "A", 6: "B"}
RWY_FIELDS = [("LATITUDE", "d"), ("LONGITUDE", "d"), ("ALTITUDE", "d"), ("HEADING", "f"), ("LENGTH", "f"),
              ("WIDTH", "f"), ("PRIMARY_NUMBER", "i"), ("PRIMARY_DESIGNATOR", "i"), ("SECONDARY_NUMBER", "i"),
              ("SECONDARY_DESIGNATOR", "i"), ("TRUE_SLOPE", "f")]
# TRUE_SLOPE: deg, + = rising along the primary (lower-numbered) direction. Checked 11/7 on LOWI 08/26:
# TRUE_SLOPE -0.12 (SLOPE +0.12), and the Inn valley falls to the east, so 08 is slightly downhill.
SLOPE_SIGN = 1.0
RWY_FMT = "<" + "".join(t for _, t in RWY_FIELDS)


def rwy_name(num, des):
    if 1 <= num <= 36:
        return f"{num:02d}{DESIG.get(des, '')}"
    return f"#{num}"


class Runway:
    """One landing direction of a runway (an 'end')."""

    def __init__(self, icao, lat, lon, alt_m, hdg, length, width, name, slope=0.0):
        self.icao, self.lat, self.lon, self.alt_m = icao, lat, lon, alt_m
        self.hdg, self.length, self.width, self.name = hdg % 360, length, width, name
        self.slope = slope  # deg, + = uphill in this landing direction

    def __repr__(self):
        return (f"{self.icao} {self.name} hdg {self.hdg:.1f} {self.length:.0f}x{self.width:.0f} m @ {self.alt_m:.1f} m"
                f" slope {self.slope:+.2f}°")

    def corners(self):
        """Pavement corners (lat, lon, alt_m): near-left, near-right, far-right, far-left (landing direction)."""
        h = math.radians(self.hdg)
        u = np.array([math.sin(h), math.cos(h)])  # along, east/north
        r = np.array([math.cos(h), -math.sin(h)])  # right
        out = []
        for a, s in ((-0.5, -0.5), (-0.5, 0.5), (0.5, 0.5), (0.5, -0.5)):
            en = u * a * self.length + r * s * self.width
            out.append(offset(self.lat, self.lon, en[0], en[1]) + (self.alt_m,))
        return out


def offset(lat, lon, e, n):
    return (lat + math.degrees(n / R_EARTH), lon + math.degrees(e / (R_EARTH * math.cos(math.radians(lat)))))


def enu(lat0, lon0, lat, lon):
    e = math.radians(lon - lon0) * R_EARTH * math.cos(math.radians(lat0))
    n = math.radians(lat - lat0) * R_EARTH
    return e, n


def ends_from_record(icao, rec):
    lat, lon, alt, hdg, length, width, pn, pd, sn, sd, slope = rec
    if not (-90 <= lat <= 90 and -180 <= lon <= 180 and 0 <= hdg <= 360.5 and 100 < length < 7000 and 5 < width < 150):
        return []
    s = SLOPE_SIGN * slope if math.isfinite(slope) and abs(slope) < 20 else 0.0
    return [Runway(icao, lat, lon, alt, hdg, length, width, rwy_name(pn, pd), s),
            Runway(icao, lat, lon, alt, hdg + 180, length, width, rwy_name(sn, sd), -s)]


def pick(ends, lat, lon, trk, current=None, max_dist=25000.0):
    """Runway end we are lined up for: landing direction within 30° of track, ahead (not yet past its far end),
    within max_dist of the threshold and within 3 km of the extended centreline. Keeps `current` while valid."""
    def score(rw):
        e, n = enu(rw.lat, rw.lon, lat, lon)  # aircraft relative to runway centre
        h = math.radians(rw.hdg)
        along = e * math.sin(h) + n * math.cos(h)  # negative = before the centre
        cross = e * math.cos(h) - n * math.sin(h)
        dang = abs((trk - rw.hdg + 180) % 360 - 180)
        if dang > 30 or along > rw.length / 2 or -along - rw.length / 2 > max_dist or abs(cross) > 3000:
            return None
        return abs(cross) + 60 * dang
    if current is not None and score(current) is not None:
        return current
    best = None
    for rw in ends:
        s = score(rw)
        if s is not None and (best is None or s < best[0]):
            best = (s, rw)
    return best[1] if best else None


# ---------------------------------------------------------------- projection helpers
def world_points_cam(points, lat0, lon0, cam_pos, R):
    """(lat, lon, alt_m) points -> camera coordinates (x right, y up, z fwd), with earth-curvature drop."""
    out = []
    for lat, lon, alt in points:
        e, n = enu(lat0, lon0, lat, lon)
        up = alt - (e * e + n * n) / (2 * R_EARTH)
        out.append(R.T @ (np.array([e, n, up]) - cam_pos))
    return out


def clip_near(poly, near=0.5):
    """Sutherland-Hodgman clip of a 3D polygon against z >= near."""
    out = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        ina, inb = a[2] >= near, b[2] >= near
        if ina:
            out.append(a)
        if ina != inb:
            t = (near - a[2]) / (b[2] - a[2])
            out.append(a + t * (b - a))
    return out


def clip_segments_outside(polyline, poly):
    """Pieces of a 2D polyline that lie outside a convex polygon (Cyrus-Beck per segment)."""
    if len(poly) < 3 or len(polyline) < 2:
        return [polyline]
    P = np.array(poly)
    L = np.asarray(polyline, dtype=float)
    if (L[:, 0].max() < P[:, 0].min() or L[:, 0].min() > P[:, 0].max()
            or L[:, 1].max() < P[:, 1].min() or L[:, 1].min() > P[:, 1].max()):
        return [polyline]  # bounding boxes do not touch
    area = sum(P[i, 0] * P[(i + 1) % len(P), 1] - P[(i + 1) % len(P), 0] * P[i, 1] for i in range(len(P)))
    sign = 1.0 if area > 0 else -1.0
    edges = [(P[i], P[(i + 1) % len(P)]) for i in range(len(P))]
    pieces, cur = [], []
    for k in range(len(polyline) - 1):
        a, b = np.array(polyline[k]), np.array(polyline[k + 1])
        d = b - a
        t0, t1, inside_possible = 0.0, 1.0, True
        for p, q in edges:
            e = q - p
            nrm = sign * np.array([-e[1], e[0]])  # inward normal
            num = nrm @ (a - p)
            den = nrm @ d
            if abs(den) < 1e-12:
                if num < 0:
                    inside_possible = False
                    break
                continue
            t = -num / den
            if den > 0:
                t0 = max(t0, t)
            else:
                t1 = min(t1, t)
            if t0 > t1:
                inside_possible = False
                break
        if not inside_possible or t1 <= t0:
            if not cur:
                cur.append(tuple(a))
            cur.append(tuple(b))
            continue
        if t0 > 0:
            if not cur:
                cur.append(tuple(a))
            cur.append(tuple(a + t0 * d))
        if cur and len(cur) > 1:
            pieces.append(cur)
        cur = []
        if t1 < 1:
            cur = [tuple(a + t1 * d), tuple(b)]
    if len(cur) > 1:
        pieces.append(cur)
    return pieces


# ---------------------------------------------------------------- SimConnect side
class FacilityClient:
    """Drives airport list + runway requests on an open simconnect_min.SimConnect (shares its handle)."""
    DEF_RWY = 9001
    REQ_LIST = 9100
    REQ_BASE = 9200

    def __init__(self, sc, log=print):
        self.sc, self.log = sc, log
        d = sc.dll
        d.SimConnect_AddToFacilityDefinition.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_char_p]
        d.SimConnect_AddToFacilityDefinition.restype = ctypes.c_long
        d.SimConnect_RequestFacilityData.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                                     ctypes.c_char_p, ctypes.c_char_p]
        d.SimConnect_RequestFacilityData.restype = ctypes.c_long
        d.SimConnect_RequestFacilitiesList_EX1.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.DWORD]
        d.SimConnect_RequestFacilitiesList_EX1.restype = ctypes.c_long
        for f in ["OPEN AIRPORT", "OPEN RUNWAY"] + [n for n, _ in RWY_FIELDS] + ["CLOSE RUNWAY", "CLOSE AIRPORT"]:
            d.SimConnect_AddToFacilityDefinition(sc.h, self.DEF_RWY, f.encode())
        self.airports = {}  # icao -> (lat, lon, alt)
        self.ends = {}  # icao -> [Runway]
        self.pending = {}  # request id -> icao
        self.next_req = self.REQ_BASE
        self.list_parts = {}

    def request_airports(self):
        self.list_parts = {}
        self.sc.dll.SimConnect_RequestFacilitiesList_EX1(self.sc.h, 0, self.REQ_LIST)

    def request_runways(self, icao):
        if icao in self.ends or icao in self.pending.values():
            return
        rid = self.next_req
        self.next_req += 1
        self.pending[rid] = icao
        self.ends[icao] = []
        self.sc.dll.SimConnect_RequestFacilityData(self.sc.h, self.DEF_RWY, rid, icao.encode(), b"")

    def handle(self, rid, raw):
        """Feed raw SimConnect messages; returns True if it consumed one."""
        if rid == RECV_AIRPORT_LIST:
            req, n, entry, outof = struct.unpack_from("<4I", raw, 12)
            if req != self.REQ_LIST:
                return False
            for i in range(n):
                o = 28 + 36 * i
                ident = raw[o:o + 9].split(b"\0", 1)[0].decode("ascii", "replace").strip()
                lat, lon, alt = struct.unpack_from("<3d", raw, o + 12)
                if ident:
                    self.airports[ident] = (lat, lon, alt)
            return True
        if rid == RECV_FACILITY_DATA:
            user_req, _u, _p, typ, _islist, _idx, _size = struct.unpack_from("<7I", raw, 12)
            icao = self.pending.get(user_req)
            if icao is None:
                return False
            if typ == FAC_RUNWAY and len(raw) >= 40 + struct.calcsize(RWY_FMT):
                rec = struct.unpack_from(RWY_FMT, raw, 40)
                ends = ends_from_record(icao, rec)
                if not ends:
                    self.log(f"[rwy] {icao} 跑道记录不合理，跳过: {rec}")
                self.ends[icao].extend(ends)
            return True
        if rid == RECV_FACILITY_DATA_END:
            (req,) = struct.unpack_from("<I", raw, 12)
            icao = self.pending.pop(req, None)
            if icao:
                self.log(f"[rwy] {icao}: " + (", ".join(r.name for r in self.ends[icao]) or "没有跑道数据"))
            return icao is not None
        return False

    def nearby(self, lat, lon, radius=30000.0):
        out = []
        for icao, (alat, alon, _alt) in self.airports.items():
            e, n = enu(lat, lon, alat, alon)
            if e * e + n * n < radius * radius:
                out.append(icao)
        return out

    def all_ends(self, icaos):
        return [rw for i in icaos for rw in self.ends.get(i, [])]
