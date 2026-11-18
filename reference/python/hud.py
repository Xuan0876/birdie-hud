"""Airbus-style HUD for the FPV overlay.

Style and geometry follow the FlyByWire A32NX HUD work in JulKem/aircraft (branch hudTest,
fbw-a32nx/src/systems/instruments/src/HUD, GPL-3.0; used here only as a visual reference):
  * 1280 x 1024 canvas, 1° = 36.5714 px (field of view 35° x 28°), centred on the aircraft boresight
  * monochrome #00ff00 with a thin black outline
  * speed tape left (10 kt = 42.5 px, labels every 20 kt, fixed index), altitude tape right (100 ft = 32 px,
    labels every 500 ft, rolling readout box), VS needle right of the altitude, roll scale arc at the top,
    pitch ladder every 5° (dashed below the horizon, end ticks towards the horizon), heading scale on the horizon.
FMA and flight director are deliberately left out.

Two kinds of symbols:
  * airframe-fixed (tapes, roll scale, readouts): drawn in canvas coordinates through a projective QTransform
    that maps the HUD plane (fixed to the aircraft body) onto the screen through the ChasePlane camera;
  * conformal (horizon, heading scale, pitch ladder, glide line, FPV, runway): world directions projected by the
    camera model, clipped to the HUD field of view.
"""
import math

import numpy as np

CW, CH = 1280.0, 1024.0
CX, CY = 640.0, 512.0
BORE_Y = 329.143  # the aircraft reference (longitudinal axis) sits 5° above the canvas centre in the reference
ONE_DEG = 36.5714
F_CANVAS = ONE_DEG * 180.0 / math.pi  # canvas px per radian at the centre -> tangent-plane distance

# tape geometry (canvas px), from the reference SVG (group offsets already applied)
SPD_LINE_X, TAPE_TOP, TAPE_BOT, TAPE_MID = 140.882, 328.865, 692.13, 510.5
SPD_PX_PER_KT = 4.25
ALT_LINE_X, ALT_PX_PER_FT = 1113.112, 0.32
ALT_BOX = (1057.3, 480.1, 1150.8, 540.9)  # readout incl. drum: tape ticks/labels hidden behind it
ROLL_C = (640.0, 329.143)
TEXT_GAIN = 1.2  # the reference sizes read small once the HUD is ~800 px wide on a 3440 screen


class HudFilters:
    """Speed trend (kt per 10 s) and lagged VS, as the reference does."""

    def __init__(self):
        self.t = None
        self.ias = None
        self.rate = 0.0
        self.vs = 0.0

    def update(self, s, t):
        ias = s.get("ias_kt", 0.0)
        vs = s.get("vy_fps", 0.0) * 60.0
        if self.t is None or not 0 < t - self.t < 0.5:
            self.t, self.ias, self.rate, self.vs = t, ias, 0.0, vs
            return
        dt = t - self.t
        a = 1 - math.exp(-dt / 1.0)
        self.rate += a * ((ias - self.ias) / dt - self.rate)
        self.vs += (1 - math.exp(-dt / 0.5)) * (vs - self.vs)
        self.t, self.ias = t, ias

    @property
    def trend10(self):
        return self.rate * 10.0


def _dir(az_deg, el_deg):
    az, el = math.radians(az_deg), math.radians(el_deg)
    return np.array([math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), math.sin(el)])


class Frame:
    """Camera + HUD plane for one frame. Screen coordinates are client pixels minus the window offset."""

    def __init__(self, p, R_cam, f, W, H, ox, oy, scale=1.0):
        import cp_camera as C
        self.p, self.R, self.f, self.W, self.H, self.ox, self.oy, self.k = p, R_cam, f, W, H, ox, oy, scale
        self.R_bc = C.camera_to_body(p["cp_yaw"], p["cp_pitch"], p.get("cp_roll", 0.0)).T  # body -> camera

    def pix(self, d_enu):
        r = self.R.T @ d_enu
        if r[2] <= 1e-3:
            return None
        return (self.W / 2 + self.f * r[0] / r[2] - self.ox, self.H / 2 - self.f * r[1] / r[2] - self.oy)

    def canvas_pix(self, u, v):
        b = np.array([(u - CX) * self.k, (BORE_Y - v) * self.k, F_CANVAS])
        c = self.R_bc @ b
        if c[2] <= 1e-3:
            return None
        return (self.W / 2 + self.f * c[0] / c[2] - self.ox, self.H / 2 - self.f * c[1] / c[2] - self.oy)

    @property
    def screen_per_canvas(self):
        return self.f / F_CANVAS * self.k


# ---------------------------------------------------------------- conformal geometry
def conformal(fr, settings, hdg_true, hdg_mag):
    """Horizon, heading ticks and pitch ladder as screen polylines (before HUD clipping)."""
    out = {"horizon": None, "hdg_ticks": [], "hdg_labels": [], "rungs": []}
    if settings.get("show_horizon", True):
        pts = [fr.pix(_dir(hdg_true + a, 0.0)) for a in np.linspace(-30, 30, 61)]
        out["horizon"] = [q for q in pts if q]
    if settings.get("show_hdg", True):
        magvar = ((hdg_true - hdg_mag + 180) % 360) - 180
        base = int(math.floor((hdg_mag - 20) / 5.0)) * 5
        for h in range(base, int(hdg_mag + 21), 5):
            az = h + magvar
            ten = h % 10 == 0
            a = fr.pix(_dir(az, 0.0))
            b = fr.pix(_dir(az, -(14.0 if ten else 8.0) / ONE_DEG))
            if a and b:
                out["hdg_ticks"].append((a, b))
            if ten:
                c = fr.pix(_dir(az, -30.0 / ONE_DEG))
                l, r = fr.pix(_dir(az - 0.5, 0.0)), fr.pix(_dir(az + 0.5, 0.0))
                if c and l and r:
                    ang = math.degrees(math.atan2(r[1] - l[1], r[0] - l[0]))
                    hh = h % 360
                    out["hdg_labels"].append((c, ang, "0" if hh == 0 else f"{hh // 10}"))
    step = settings.get("ladder_step_deg", 5.0) or 0
    if step > 0:
        inner, outer = (CX - 518.26) / ONE_DEG, (CX - 447.16) / ONE_DEG  # 3.33° .. 5.27° either side
        tick = 11.0 / ONE_DEG
        kmax = int(min(settings.get("ladder_max_deg", 30.0), 30.0) / step + 1e-6)
        for k in range(-kmax, kmax + 1):
            if k == 0:
                continue
            e = k * step
            segs = []
            for side in (-1, 1):
                if e > 0:  # solid rung, end tick down towards the horizon
                    pts = [fr.pix(_dir(hdg_true + side * a, e)) for a in np.linspace(inner, outer, 6)]
                    end = fr.pix(_dir(hdg_true + side * outer, e - tick))
                    segs.append([q for q in pts + [end] if q])
                else:  # dashed: 4 dashes of 12 px every 19.7 px, last one with an end tick up
                    for j in range(4):
                        a0 = inner + j * 19.7 / ONE_DEG
                        a1 = a0 + 12.0 / ONE_DEG
                        pts = [fr.pix(_dir(hdg_true + side * a0, e)), fr.pix(_dir(hdg_true + side * a1, e))]
                        if j == 3:
                            pts.append(fr.pix(_dir(hdg_true + side * a1, e + tick)))
                        segs.append([q for q in pts if q])
            labels = []
            for side in (-1, 1):
                q = fr.pix(_dir(hdg_true + side * (outer + 2.5 / ONE_DEG), e))
                l, r = fr.pix(_dir(hdg_true - 0.5, e)), fr.pix(_dir(hdg_true + 0.5, e))
                if q and l and r:
                    ang = math.degrees(math.atan2(r[1] - l[1], r[0] - l[0]))
                    labels.append((q, ang, side, f"{e:.0f}"))
            out["rungs"].append((e, [s for s in segs if len(s) > 1], labels))
    out["minor"] = []
    if settings.get("show_pitch_minor", True):  # PFD-style short 2.5° marks, centred on the heading
        half = 12.0 / ONE_DEG
        top = min(settings.get("ladder_max_deg", 30.0), 30.0)
        for j in range(-int(top / 2.5), int(top / 2.5) + 1):
            e = j * 2.5
            if j == 0 or (step > 0 and abs(e / step - round(e / step)) < 1e-6):
                continue  # horizon / a rung is already there
            a, b = fr.pix(_dir(hdg_true - half, e)), fr.pix(_dir(hdg_true + half, e))
            if a and b:
                out["minor"].append([a, b])
    return out


# ---------------------------------------------------------------- painting
_FONT_FAMILY = None
_FONTS = {}


def _font(px):
    """Cached font by pixel size (resolving the family once: exactMatch is slow)."""
    global _FONT_FAMILY
    from PyQt6.QtGui import QFont, QFontMetricsF
    key = int(round(px * 4))
    hit = _FONTS.get(key)
    if hit is None:
        if _FONT_FAMILY is None:
            _FONT_FAMILY = "Bahnschrift SemiCondensed" if QFont("Bahnschrift SemiCondensed").exactMatch() else "Bahnschrift"
        f = QFont(_FONT_FAMILY)
        f.setPixelSize(max(1, key // 4))
        hit = (f, QFontMetricsF(f))
        _FONTS[key] = hit
    return hit


class Homography:
    """Projective map canvas -> screen (numpy, no Qt objects in the hot path)."""

    def __init__(self, qt):
        self.qt = qt
        self.M = np.array([[qt.m11(), qt.m21(), qt.m31()],
                           [qt.m12(), qt.m22(), qt.m32()],
                           [qt.m13(), qt.m23(), qt.m33()]])

    def pts(self, pts):
        a = np.asarray(pts, dtype=float)
        h = a @ self.M[:, :2].T + self.M[:, 2]
        return h[:, :2] / h[:, 2:3]

    def pt(self, x, y):
        m = self.M
        w = m[2, 0] * x + m[2, 1] * y + m[2, 2]
        return ((m[0, 0] * x + m[0, 1] * y + m[0, 2]) / w, (m[1, 0] * x + m[1, 1] * y + m[1, 2]) / w)


class Batch:
    """Collects HUD geometry in screen coordinates (point lists), painted later either with QPainter (offline
    snapshots) or by the GPU renderer (hudgfx.dll, live): every black outline first, then every green stroke /
    fill / glyph. Canvas-space input is mapped through the HUD-plane homography so it stays glued to the airframe."""

    def __init__(self, T=None, sc=1.0):
        self.H = Homography(T) if T is not None else None
        self.sc = sc
        self.lines = {}  # (width_px, dashed) -> [(points, closed)]
        self.fills = []  # point arrays
        self.texts = []  # (x, y, px, string, align, angle, dy) in screen space

    def line(self, pts, w, canvas=False, close=False, dash=False):
        pts = [q for q in pts if q is not None]
        if len(pts) < 2:
            return
        if canvas and self.H is not None:
            pts = self.H.pts(pts)
        width = w * self.sc if canvas else w
        key = (round(max(1.2, width), 1), dash)
        self.lines.setdefault(key, []).append((pts, close))

    def fill(self, pts, canvas=False):
        if canvas and self.H is not None:
            pts = self.H.pts(pts)
        self.fills.append(pts)

    def text(self, x, y, s, px, align="mid", canvas=False, rot=None):
        """canvas=True: (x, y, px) in canvas units, placed through the homography (glyphs themselves are not
        perspective-warped). rot=(angle, dy): screen rotation about (x, y), then dy."""
        if canvas and self.H is not None:
            x, y = self.H.pt(x, y)
            px = px * self.sc * TEXT_GAIN
        ang, dy = rot if rot else (0.0, 0.0)
        self.texts.append((x, y, px, s, align, ang, dy))

    # ---- GPU renderer: flat float command list (see native/hudgfx.cpp)
    def emit(self, cmds, strs, pos):
        """Append this batch to `cmds` (floats); strings go to `strs`, `pos` = [current string length]."""
        for (w, dash), items in self.lines.items():
            for pts, closed in items:
                a = np.asarray(pts, dtype=float).ravel()
                cmds.extend((1.0, w, 1.0 if dash else 0.0, 1.0 if closed else 0.0, len(a) // 2))
                cmds.extend(a.tolist())
        for pts in self.fills:
            a = np.asarray(pts, dtype=float).ravel()
            cmds.extend((2.0, len(a) // 2))
            cmds.extend(a.tolist())
        for x, y, px, txt, align, ang, dy in self.texts:
            cmds.extend((3.0, x, y, px, {"start": 0.0, "mid": 1.0, "end": 2.0}[align], ang, dy, pos[0], len(txt)))
            strs.append(txt)
            pos[0] += len(txt)
        cmds.append(6.0)

    # ---- QPainter (offline snapshots, fallback)
    def paint(self, qp, green, black):
        from PyQt6.QtCore import QPointF, Qt
        from PyQt6.QtGui import QColor, QPainterPath, QPen

        def path_of(pts, closed):
            path = QPainterPath(QPointF(float(pts[0][0]), float(pts[0][1])))
            for q in pts[1:]:
                path.lineTo(QPointF(float(q[0]), float(q[1])))
            if closed:
                path.closeSubpath()
            return path

        def pen(color, w, dash=False):
            p = QPen(color, w)
            p.setCapStyle(Qt.PenCapStyle.FlatCap if dash else Qt.PenCapStyle.SquareCap)
            p.setJoinStyle(Qt.PenJoinStyle.BevelJoin)
            if dash:
                p.setDashPattern([6.0, 4.0])
            return p

        paths = {}
        for key, items in self.lines.items():
            pp = QPainterPath()
            for pts, closed in items:
                pp.addPath(path_of(pts, closed))
            paths[key] = pp
        fills = QPainterPath()
        for pts in self.fills:
            fills.addPath(path_of(pts, True))
        outline = 2.4
        for (w, dash), path in paths.items():
            p = pen(black, w + outline, dash)
            if dash:
                p.setDashPattern([6.0 * w / (w + outline), 4.0 * w / (w + outline)])
            qp.strokePath(path, p)
        if not fills.isEmpty():
            qp.strokePath(fills, pen(black, outline))
        for (w, dash), path in paths.items():
            qp.strokePath(path, pen(green, w, dash))
        if not fills.isEmpty():
            qp.fillPath(fills, green)
        halo = QColor(0, 0, 0, 120)
        for x, y, px, txt, align, ang, dy in self.texts:
            f, fm = _font(px)
            w = fm.horizontalAdvance(txt)
            dx = {"start": 0.0, "mid": -w / 2, "end": -w}[align]
            qp.setFont(f)
            if ang:
                qp.save()
                qp.translate(x, y)
                qp.rotate(ang)
                x0, y0 = dx, dy
            else:
                x0, y0 = x + dx, y + dy
            qp.setPen(halo)
            for ox, oy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                qp.drawText(QPointF(x0 + ox, y0 + oy), txt)
            qp.setPen(green)
            qp.drawText(QPointF(x0, y0), txt)
            if ang:
                qp.restore()


def canvas_transform(fr):
    from PyQt6.QtCore import QPointF
    from PyQt6.QtGui import QPolygonF, QTransform
    src = [(0, 0), (CW, 0), (CW, CH), (0, CH)]
    dst = [fr.canvas_pix(u, v) for u, v in src]
    if any(q is None for q in dst):
        return None, None
    t = QTransform()
    ok = QTransform.quadToQuad(QPolygonF([QPointF(*q) for q in src]), QPolygonF([QPointF(*q) for q in dst]), t)
    return (t if ok else None), dst


def build(fr, s, conf, filt, settings, bird=None, glide=None, glide_deg=3.0, rwy_poly=None, rwy_label=None,
          flash=None, fd=None, fpa=None, vs_fpm=None, flare=None):
    """Whole HUD as a scene: {'main': Batch, 'clip': HUD quad, 'drum': (window quad, Batch) or None, 'flash'}."""
    T, quad = canvas_transform(fr)
    sc = fr.screen_per_canvas
    B = Batch(T, sc)
    w = max(1.5, 3.0 * sc)

    # ---------------- conformal (screen space)
    if rwy_poly and settings.get("show_runway", True):
        B.line(rwy_poly, w, close=True)
        if rwy_label:
            bx, by = max(rwy_poly, key=lambda q: q[1])
            cx = sum(q[0] for q in rwy_poly) / len(rwy_poly)
            B.text(0.5 * (bx + cx), by + 22 * sc + 8, rwy_label, 22 * sc + 2)
    for seg in conf.get("horizon_pieces") or ([conf["horizon"]] if conf.get("horizon") else []):
        B.line(seg, w)
    for a, b in conf.get("hdg_ticks", []):
        B.line([a, b], max(1.3, 2.4 * sc))
    for (x, y), ang, txt in conf.get("hdg_labels", []):
        B.text(x, y, txt, 26 * sc, rot=(ang, 10 * sc))
    for e, segs, labels in conf.get("rungs", []):
        for seg in segs:
            B.line(seg, w)
        for (x, y), ang, side, txt in labels:
            B.text(x, y, txt, 26 * sc, "end" if side < 0 else "start", rot=(ang, 9 * sc))
    for seg in conf.get("minor", []):
        B.line(seg, max(1.2, 2.2 * sc))
    if glide and settings.get("show_glide", True):
        for seg in glide:
            B.line(seg, w, dash=True)
        ex, ey = glide[-1][-1]
        B.text(ex + 6, ey + 8 * sc, f"-{glide_deg:.1f}", 24 * sc, "start")
    if bird and settings.get("show_bird", True):
        x, y = bird
        r = 13.0 * sc + 2
        circ = [(x + r * math.cos(a), y + r * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 25)]
        bw = max(1.8, 3.6 * sc)
        B.line(circ, bw, close=True)
        B.line([(x - r, y), (x - 3 * r, y)], bw)
        B.line([(x + r, y), (x + 3 * r, y)], bw)
        B.line([(x, y - r), (x, y - 2.1 * r)], bw)
        if fpa is not None and settings.get("show_fpa_num", True):  # current flight path angle, right of the wing
            v = round(fpa, 1)
            txt = ("−" if v < 0 else "") + f"{abs(v):.1f}"
            B.text(x + 3 * r + 7, y + 8 * sc + 1, txt, 24 * sc + 2, "start")
        if flare and flare["n"] and flare["on"]:  # flare helper: blinking + (on profile) / ++ (above it)
            arm = 0.55 * r
            yc = y - 2.1 * r - 10 * sc - arm - 2
            xs = [x] if flare["n"] == 1 else [x - arm - 5 * sc - 2, x + arm + 5 * sc + 2]
            for xc in xs:
                B.line([(xc - arm, yc), (xc + arm, yc)], bw)
                B.line([(xc, yc - arm), (xc, yc + arm)], bw)
        if vs_fpm is not None and settings.get("show_vs_num", True):  # vertical speed (fpm), left of the wing
            v = int(round(vs_fpm / 10.0)) * 10
            txt = ("−" if v < 0 else "") + f"{abs(v)}"
            B.text(x - 3 * r - 7, y + 8 * sc + 1, txt, 24 * sc + 2, "end")
    if fd and fd.get("mode") == "fpa" and bird and settings.get("show_fd", True):
        _fpd(B, bird, fd, sc)

    # ---------------- airframe-fixed (canvas space)
    drum = None
    if T is not None:
        _speed(B, s, filt, settings)
        d = _altitude(B, s, filt, settings)
        _roll(B, s, settings)
        _aircraft_ref(B, settings)
        if fd and settings.get("show_fd", True) and fd.get("mode", "bars") == "bars":
            _fd_bars(B, fd)
        _misc(B, s, settings)
        if d:  # rolling tens of the altitude readout: clipped to its window
            rect, digits = d
            D = Batch(T, sc)
            for x, y, txt in digits:
                D.text(x, y, txt, 26, canvas=True)
            drum = (B.H.pts(rect), D)
    F = None
    if flash:
        F = Batch()
        F.text(14, 26, flash, 15, "start")
    return {"main": B, "clip": quad, "drum": drum, "flash": F}


def paint(qp, fr, s, conf, filt, settings, **kw):
    """QPainter rendering of build() (offline snapshots)."""
    from PyQt6.QtCore import QPointF, Qt
    from PyQt6.QtGui import QColor, QPainterPath, QPolygonF
    scene = build(fr, s, conf, filt, settings, **kw)
    green, black = QColor(0, 255, 0), QColor(0, 0, 0, 190)
    qp.setRenderHint(qp.RenderHint.Antialiasing, True)
    qp.setOpacity(max(0.05, min(1.0, settings.get("opacity", 1.0))))

    def poly(pts):
        path = QPainterPath()
        path.addPolygon(QPolygonF([QPointF(float(x), float(y)) for x, y in pts]))
        path.closeSubpath()
        return path

    qp.save()
    if scene["clip"]:  # nothing outside the HUD field of view
        qp.setClipPath(poly(scene["clip"]))
    scene["main"].paint(qp, green, black)
    if scene["drum"]:
        win, D = scene["drum"]
        qp.setClipPath(poly(win), Qt.ClipOperation.IntersectClip)
        D.paint(qp, green, black)
    qp.restore()
    if scene["flash"]:
        scene["flash"].paint(qp, green, black)


def commands(scene, opacity=1.0):
    """GPU renderer input for a scene (None = blank frame): (float list, string)."""
    cmds, strs, pos = [], [], [0]
    if scene is None:
        return cmds, ""
    a = max(0.05, min(1.0, opacity))
    cmds.extend((10.0, 0.0, 1.0, 0.0, a,  # green (straight alpha: Direct2D premultiplies brush colours itself)
                 0.0, 0.0, 0.0, 0.745 * a,  # outline black (190/255)
                 0.0, 0.0, 0.0, 0.47 * a,  # text halo (120/255)
                 2.4))

    def clip(pts):
        v = np.asarray(pts, dtype=float).ravel()
        cmds.extend((4.0, len(v) // 2))
        cmds.extend(v.tolist())

    if scene.get("main") is not None:
        if scene["clip"]:
            clip(scene["clip"])
        scene["main"].emit(cmds, strs, pos)
        if scene["drum"]:
            win, D = scene["drum"]
            clip(win)
            D.emit(cmds, strs, pos)
            cmds.append(5.0)
        if scene["clip"]:
            cmds.append(5.0)
    if scene.get("flash"):
        scene["flash"].emit(cmds, strs, pos)
    return cmds, "".join(strs)


def _speed(B, s, filt, settings):
    if not settings.get("show_speed", True):
        return
    ias = s.get("ias_kt", 0.0)
    w = 3.0
    B.line([(SPD_LINE_X, TAPE_TOP), (SPD_LINE_X, TAPE_BOT)], w, canvas=True)
    B.line([(68.1, TAPE_TOP), (161.0, TAPE_TOP)], w, canvas=True)
    B.line([(68.1, TAPE_BOT), (161.0, TAPE_BOT)], w, canvas=True)
    lo = int(math.floor((ias - 45) / 10.0)) * 10
    for v in range(max(30, lo), int(ias + 46), 10):
        y = TAPE_MID - (v - ias) * SPD_PX_PER_KT
        if TAPE_TOP + 4 < y < TAPE_BOT - 4:
            B.line([(SPD_LINE_X - 12.0, y), (SPD_LINE_X, y)], w, canvas=True)
            if v % 20 == 0 and TAPE_TOP + 14 < y < TAPE_BOT - 6:
                B.text(94.0, y + 9.27, f"{v}", 26, canvas=True)
    # fixed speed index: bar across the scale + pointer
    B.fill([(119.47, 509.5), (119.47, 512.58), (147.3, 512.58), (160.57, 517.47), (160.57, 504.62), (147.3, 509.5)],
           canvas=True)
    tr = filt.trend10 if filt else 0.0
    if abs(tr) > 2.0:
        tr = max(-42.0, min(42.0, tr))
        y1 = TAPE_MID - tr * SPD_PX_PER_KT
        sg = 1 if tr > 0 else -1
        B.line([(168.0, TAPE_MID), (168.0, y1)], w, canvas=True)
        B.line([(161.0, y1 + sg * 10), (168.0, y1), (175.0, y1 + sg * 10)], w, canvas=True)
    mach = s.get("mach", 0.0)
    if mach >= 0.5:
        B.text(104.0, 730.0, f".{int(round(mach * 100)) % 100:02d}", 28, canvas=True)
    gs = s.get("gs_kt")
    if gs is not None and settings.get("show_gs", True):
        B.text(68.0, 300.0, f"GS {gs:.0f}", 24, "start", canvas=True)
        _wind(B, s)


def _wind(B, s):
    """Wind indicator as on the reference HUD (= the ND one, at translate(250 200) + (23 58)): 'DDD / SS' and an
    arrow showing where the wind blows, relative to the nose (up = straight ahead)."""
    wd, wk = s.get("wind_dir"), s.get("wind_kt")
    if wd is None or wk is None:
        return
    hdg_mag = s.get("hdg_mag", s["hdg_true"])
    magvar = ((s["hdg_true"] - hdg_mag + 180) % 360) - 180
    wd_mag = (wd - magvar) % 360
    x0, y0 = 273.0, 258.0
    B.text(x0 + 25, y0, f"{round(wd_mag) % 360:03d}", 22, "end", canvas=True)
    B.text(x0 + 33, y0 - 1, "/", 20, "mid", canvas=True)
    B.text(x0 + 42, y0, f"{round(wk):d}", 22, "start", canvas=True)
    if wk > 2:
        rot = math.radians((round(wd_mag) - round(hdg_mag) + 180) % 360)
        cx, cy = x0 + 3, y0 + 10 + 15  # rotation centre = middle of the 30 px shaft
        cr, sr = math.cos(rot), math.sin(rot)

        def r(px, py):  # arrow drawn pointing up (local y up the screen), turned clockwise by rot
            dx, dy = px, py - 15
            return (cx + dx * cr - dy * sr, cy + dx * sr + dy * cr)
        B.line([r(0, 30), r(0, 0)], 3.0, canvas=True)
        B.line([r(-6.5, 12), r(0, 0), r(6.5, 12)], 3.0, canvas=True)


def _altitude(B, s, filt, settings):
    w = 3.0
    drum = None
    alt = s.get("ind_alt_ft", s.get("alt_ft", 0.0))
    if settings.get("show_alt", True):
        B.line([(1057.4, TAPE_BOT), (1133.2, TAPE_BOT)], w, canvas=True)
        B.line([(ALT_LINE_X, TAPE_TOP), (ALT_LINE_X, TAPE_BOT)], w, canvas=True)
        B.line([(1057.4, TAPE_TOP), (1133.2, TAPE_TOP)], w, canvas=True)
        bx0, by0, bx1, by1 = ALT_BOX
        lo = int(math.floor((alt - 650) / 100.0)) * 100
        for v in range(lo, int(alt + 651), 100):
            y = TAPE_MID - (v - alt) * ALT_PX_PER_FT
            if not TAPE_TOP + 4 < y < TAPE_BOT - 4 or by0 - 4 < y < by1 + 4:
                continue
            B.line([(ALT_LINE_X - 8.56, y), (ALT_LINE_X, y)], w, canvas=True)
            if v % 500 == 0 and TAPE_TOP + 14 < y < TAPE_BOT - 6 and not by0 - 12 < y < by1 + 18:
                B.text(1081.0, y + 8.5, f"{v // 100:d}" if v >= 0 else f"-{abs(v) // 100:d}", 26, canvas=True)
        # readout: hundreds and above in the left part, rolling tens drum on the right
        B.line([(1113.1, 529.6), (1057.3, 529.6), (1057.3, 491.4), (1113.1, 491.4), (1113.1, 480.1), (1150.8, 480.1),
                (1150.8, 540.9), (1113.1, 540.9), (1113.1, 529.6)], w, canvas=True)
        a = abs(alt)
        B.text(1110.0, 522.0, ("-" if alt < 0 else "") + f"{int(a // 100):d}", 34, "end", canvas=True)
        tens = a % 100
        base = int(tens // 20) * 20
        frac = (tens - base) / 20.0
        digits = [(1132.0, 520.0 + (frac - j) * 27.0, f"{(base + 20 * j) % 100:02d}") for j in (-1, 0, 1, 2)]
        drum = ([(1114.6, 481.6), (1149.3, 481.6), (1149.3, 539.4), (1114.6, 539.4)], digits)
        baro = s.get("baro_mb")
        if baro:
            B.text(1095.0, 730.0, f"{baro:.0f}", 26, canvas=True)
    if settings.get("show_vs", True):
        vs = filt.vs if filt else s.get("vy_fps", 0.0) * 60
        av, sg = abs(vs), (1 if vs >= 0 else -1)
        if av < 1000:
            yo = vs / 1000 * -136.1
        elif av < 2000:
            yo = (vs - sg * 1000) / 1000 * -50.5 - sg * 136.1
        elif av < 6000:
            yo = (vs - sg * 2000) / 4000 * -50.5 - sg * 186.6
        else:
            yo = sg * -237.0
        B.fill([(1165, 508.5), (1185, 508.5), (1185, 512.5), (1165, 512.5)], canvas=True)
        B.line([(1220.0, 510.5 + 0.26 * yo), (1183.0, 510.5 + yo)], 4.0, canvas=True)
        if av >= 200:
            n = int(round(av / 100))
            txt = ("-" if vs < 0 else "") + (f"0{n}" if n < 10 else f"{n}")
            B.text(1197.0 + (0 if vs > 0 else -5), 518.0 + yo - sg * 14, txt, 22, canvas=True)
    return drum


def _roll(B, s, settings):
    if not settings.get("show_roll", True):
        return
    w = 3.0
    cx, cy = ROLL_C
    a0, a1 = math.radians(155.16), math.radians(24.84)
    B.line([(cx + 190 * math.cos(a), cy - 190 * math.sin(a)) for a in np.linspace(a0, a1, 40)], w, canvas=True)
    for a, b in (((735.2, 164), (749.3, 139.5)), ((705.1, 150), (712.8, 129.3)), ((673, 141.5), (677, 119.3)),
                 ((544.8, 164), (530.7, 139.5)), ((574.9, 150), (567.2, 129.3)), ((607, 141.5), (603, 119.3)),
                 ((774.5, 194.5), (794.5, 174.5)), ((505.5, 194.5), (485.5, 174.5)),
                 ((813, 249), (832, 240)), ((467, 249), (448, 240))):
        B.line([a, b], w, canvas=True)
    B.line([(640, 138.44), (652.22, 118.24), (627.79, 118.24)], w, canvas=True, close=True)
    # roll index + sideslip, turning with the horizon (SimConnect bank: + = left wing down -> clockwise)
    ang = math.radians(s.get("bank_deg", 0.0))
    ca, sa = math.cos(ang), math.sin(ang)

    def rot(q):
        x, y = q[0] - cx, q[1] - cy
        return (cx + x * ca - y * sa, cy + x * sa + y * ca)
    B.line([rot(q) for q in ((640.18, 140), (630.18, 161.89), (650.18, 161.89))], w, canvas=True, close=True)
    slip = max(-30.0, min(30.0, s.get("beta_deg", 0.0) * 6.0))
    B.line([rot(q) for q in ((629 + slip, 164.85), (621 + slip, 181), (659 + slip, 181.07), (651 + slip, 164.85))],
           w, canvas=True, close=True)


def _aircraft_ref(B, settings):
    """Fixed aircraft reference ("in air" symbol of the reference): wings + small square on the boresight.
    Read pitch attitude against the pitch ladder and bank against the roll scale."""
    if not settings.get("show_att", True):
        return
    w = 3.4
    B.line([(625, 335), (625, 329), (595, 329)], w, canvas=True)
    B.line([(637, 332), (643, 332), (643, 326), (637, 326)], w, canvas=True, close=True)
    B.line([(655, 335), (655, 329), (685, 329)], w, canvas=True)


def _fd_bars(B, fd):
    """FSLabs' own FD bars, read off the popped-out PFD (pfd_reader.py), drawn around the aircraft reference
    with the PFD's offsets taken 1:1 in degrees. Pitch bar above the symbol = fly up; roll bar right = roll right."""
    w = 3.4
    half = 150.0
    if fd.get("pitch") is not None:
        y = BORE_Y - max(-10.0, min(10.0, fd["pitch"])) * ONE_DEG
        B.line([(CX - half, y), (CX - 14, y)], w, canvas=True)
        B.line([(CX + 14, y), (CX + half, y)], w, canvas=True)
    if fd.get("roll") is not None:
        x = CX + max(-10.0, min(10.0, fd["roll"])) * ONE_DEG
        B.line([(x, BORE_Y - half), (x, BORE_Y - 14)], w, canvas=True)
        B.line([(x, BORE_Y + 14), (x, BORE_Y + half)], w, canvas=True)


def _fpd(B, bird, fd, sc):
    """TRK-FPA mode flight path director: a ring a bit larger than the FPV circle (user 11/6: fly the bird's circle
    into it), placed off the FPV like the A32NX HUD cue: roll order x 13 px, pitch order x 1° (36.57 px), canvas
    units. fd['roll'] = FSLabs' FPD tilt on the PFD (deg, + = roll right), fd['pitch'] = FPD above the PFD bird
    (deg, + = fly up)."""
    x, y = bird
    roll = max(-45.0, min(45.0, fd.get("roll") or 0.0))
    pitch = max(-10.0, min(7.5, fd.get("pitch") or 0.0))
    cx, cy = x + roll * 13.0 * sc, y - pitch * ONE_DEG * sc
    r = 1.6 * (13.0 * sc + 2)  # bird circle is 13 sc + 2
    circ = [(cx + r * math.cos(a), cy + r * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 33)]
    B.line(circ, max(1.8, 3.0 * sc), close=True)


def _misc(B, s, settings):
    ra = s.get("radio_alt_ft")
    if ra is not None and settings.get("show_alt", True) and 0 <= ra <= 2500 and not s.get("on_ground", 0):
        B.text(1095.0, 772.0, f"RA {ra:.0f}", 26, canvas=True)  # under the altitude tape, clear of the runway
