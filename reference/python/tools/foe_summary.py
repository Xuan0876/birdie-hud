"""Summarise foe_check.py output: error of the image FOE (method A, derotated) against the data bird.

    python foe_summary.py <name> [<name> ...]      # reads ../foe_out/<name>.csv, writes <name>_plot.png + summary json
"""
import csv
import json
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "foe_out")
PX_PER_DEG_3440 = 1298.0 * math.tan(math.radians(1))  # near screen centre, default cockpit zoom
BLUE, ORANGE = "#2a78d6", "#eb6834"
plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False


def rolling_median(t, v, win=1.0):
    out = np.full(len(v), np.nan)
    for i, ti in enumerate(t):
        m = (t > ti - win / 2) & (t < ti + win / 2) & ~np.isnan(v)
        if m.sum() >= 3:
            out[i] = np.median(v[m])
    return out


def stats(v):
    v = v[~np.isnan(v)]
    return {"n": int(len(v)), "median": round(float(np.median(v)), 3),
            "p10": round(float(np.percentile(v, 10)), 3), "p90": round(float(np.percentile(v, 90)), 3),
            "mad": round(float(np.median(np.abs(v - np.median(v)))), 3)}


def main():
    for name in sys.argv[1:]:
        with open(os.path.join(OUT, name + ".csv"), encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
        g = lambda k: np.array([float(r[k]) if r[k] not in ("", None) else np.nan for r in rows])  # noqa: E731
        t, agl = g("vs"), g("agl_ft")
        dx, dy = g("A_dx_deg"), g("A_dy_deg")
        sx, sy = rolling_median(t, dx), rolling_median(t, dy)
        lag_file = os.path.join(OUT, name + "_lag.txt")
        lag = open(lag_file, encoding="utf-8").read().strip().splitlines()[-1] if os.path.exists(lag_file) else ""
        summ = {"name": name, "lag": lag, "px_per_deg_at_3440": round(PX_PER_DEG_3440, 1),
                "raw_dx": stats(dx), "raw_dy": stats(dy), "smoothed_1s_dx": stats(sx), "smoothed_1s_dy": stats(sy),
                "bands": {}}
        for lo, hi in [(1000, 2000), (500, 1000), (200, 500), (50, 200)]:
            m = (agl >= lo) & (agl < hi)
            if m.sum() > 5:
                summ["bands"][f"{lo}-{hi}ft"] = {"dx": stats(sx[m]), "dy": stats(sy[m])}
        with open(os.path.join(OUT, name + "_summary.json"), "w", encoding="utf-8") as f:
            json.dump(summ, f, ensure_ascii=False, indent=1)

        fig, axes = plt.subplots(2, 1, figsize=(11, 6), sharex=True)
        for ax, raw, sm, lab in [(axes[0], dx, sx, "水平（正 = 图像扩张中心在小鸟右边）"),
                                 (axes[1], dy, sy, "垂直（正 = 图像扩张中心在小鸟上方）")]:
            ax.scatter(t, raw, s=4, color=BLUE, alpha=0.25, linewidths=0, label="单个帧对")
            ax.plot(t, sm, color=BLUE, lw=2, label="1 秒滑动中位数")
            ax.axhline(0, color="#52514e", lw=1)
            for d in (-0.5, 0.5):
                ax.axhline(d, color="#c3c2b7", lw=0.8, ls="--")
            ax.set_ylim(-2.5, 2.5)
            ax.set_ylabel("偏差（度）")
            ax.set_title(lab, loc="left", fontsize=10)
            ax.grid(False)
            for s in ("top", "right"):
                ax.spines[s].set_visible(False)
        axes[0].legend(loc="upper right", frameon=False, fontsize=9)
        # AGL as tick labels along the bottom instead of a second y-axis
        ticks = np.linspace(t[0], t[-1], 9)
        axes[1].set_xticks(ticks)
        axes[1].set_xticklabels([f"{x:.0f}s\n{np.interp(x, t, agl):.0f}ft" for x in ticks], fontsize=8)
        axes[1].set_xlabel("录像时间 / 离地高度")
        fig.suptitle(f"{name}：图像扩张中心 − 数据小鸟（±0.5° 虚线 ≈ ±{0.5 * PX_PER_DEG_3440:.0f} px @3440）  {lag}",
                     fontsize=11, x=0.01, ha="left")
        fig.tight_layout()
        fig.savefig(os.path.join(OUT, name + "_plot.png"), dpi=110)
        print(json.dumps(summ, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
