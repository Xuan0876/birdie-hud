"""Zoom -> focal length calibration, step 2: measure how much the picture scales between the captured zoom levels.

A zoom change in a paused sim only changes the field of view, so every capture is the reference capture scaled about
the picture centre: s = f(z) / f(z_ref). The scale comes from ORB feature matches (cockpit + scenery) fitted with
a RANSAC similarity transform. The anchor f(z_ref) is Birdie's verified point (1298 px at 3440 px width for zoom
0.49725, checked against video on the A321) when a capture is close to it, else the current linear model.

    python -I tools/zoom_calib/analyze.py <folder> [--ref 0.497]
"""
import argparse
import glob
import math
import os
import re

import cv2
import numpy as np

K_LINEAR = 1298.0 / 0.49725  # current model: f = zoom * K * width / 3440


def load(folder):
    out = []
    for p in glob.glob(os.path.join(folder, "zoom_*.png")):
        m = re.search(r"zoom_([0-9.]+)\.png$", p)
        if m:
            out.append((float(m.group(1)), p))
    return sorted(out)


def scale_between(ref_img, img, orb, bf):
    k1, d1 = orb.detectAndCompute(ref_img, None)
    k2, d2 = orb.detectAndCompute(img, None)
    if d1 is None or d2 is None:
        return None, 0
    matches = bf.knnMatch(d1, d2, k=2)
    good = [m for m, n in (x for x in matches if len(x) == 2) if m.distance < 0.75 * n.distance]
    if len(good) < 12:
        return None, len(good)
    src = np.float32([k1[m.queryIdx].pt for m in good])
    dst = np.float32([k2[m.trainIdx].pt for m in good])
    M, inl = cv2.estimateAffinePartial2D(src, dst, method=cv2.RANSAC, ransacReprojThreshold=2.0, maxIters=5000,
                                         confidence=0.999)
    if M is None:
        return None, 0
    s = math.hypot(M[0, 0], M[1, 0])
    # where the scaling centre is (should be the picture centre for a pure FOV change)
    h, w = ref_img.shape[:2]
    A = M[:, :2]
    t = M[:, 2]
    try:
        c = np.linalg.solve(np.eye(2) - A, t) if abs(s - 1) > 0.02 else None
    except np.linalg.LinAlgError:
        c = None
    return (s, c, (w, h)), int(inl.sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--ref", type=float, default=0.49725)
    a = ap.parse_args()
    caps = load(a.folder)
    if len(caps) < 2:
        raise SystemExit("need at least two captures")
    z_ref, p_ref = min(caps, key=lambda c: abs(c[0] - a.ref))
    ref = cv2.imread(p_ref, cv2.IMREAD_GRAYSCALE)
    h, w = ref.shape[:2]
    f_ref = z_ref * K_LINEAR * w / 3440.0
    anchored = abs(z_ref - 0.49725) < 0.01
    print(f"reference zoom {z_ref:.3f}, {w}x{h}, f_ref {f_ref:.1f} px "
          f"({'verified anchor' if anchored else 'linear model, NOT the verified point'})")
    orb = cv2.ORB_create(nfeatures=8000)
    bf = cv2.BFMatcher(cv2.NORM_HAMMING)
    rows = []
    for z, p in caps:
        img = cv2.imread(p, cv2.IMREAD_GRAYSCALE)
        if z == z_ref:
            res, n = (1.0, None, (w, h)), -1
        else:
            res, n = scale_between(ref, img, orb, bf)
        if res is None:
            print(f"zoom {z:.3f}: no fit ({n} matches)")
            continue
        s, c, _ = res
        f = f_ref * s
        hfov = 2 * math.degrees(math.atan(w / 2 / f))
        vfov = 2 * math.degrees(math.atan(h / 2 / f))
        lin = z * K_LINEAR * w / 3440.0
        cs = f"centre ({c[0]:.0f},{c[1]:.0f})" if c is not None else "centre -"
        print(f"zoom {z:.3f}: scale {s:.4f}  f {f:7.1f} px  (linear model {lin:7.1f}, error {100 * (lin / f - 1):+5.1f} %)"
              f"  hfov {hfov:5.1f}  vfov {vfov:5.1f}  inliers {n}  {cs}")
        rows.append((z, f, hfov, vfov))
    if len(rows) >= 3:
        z = np.array([r[0] for r in rows])
        f = np.array([r[1] for r in rows]) * 3440.0 / w
        A = np.vstack([z, np.ones_like(z)]).T
        (b, c0), *_ = np.linalg.lstsq(A, f, rcond=None)
        print(f"fit f(3440 px wide) = {b:.1f} * zoom + {c0:.1f}   (current: {K_LINEAR:.1f} * zoom)")
        for name, x in (("tan(hfov/2)", np.tan(np.radians([r[2] for r in rows]) / 2)),
                        ("hfov deg", np.array([r[2] for r in rows]))):
            A2 = np.vstack([z, np.ones_like(z)]).T
            (b2, c2), res, *_ = np.linalg.lstsq(A2, x, rcond=None)
            print(f"fit {name} = {b2:.4f} * zoom + {c2:.4f}   residual {float(res[0]) if len(res) else 0:.2e}")


if __name__ == "__main__":
    main()
