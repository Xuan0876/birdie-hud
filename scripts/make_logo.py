"""Writes the README logo: res/logo-dark.svg (for dark pages) and res/logo-light.svg (for light pages).

The wordmark of the tray card in the default "tool" look: "Birdie" in Birdie Sans (Jost), its i dotted with the bird
(the flight path vector). Letters are outlined from panel/.../fonts/birdie-sans.ttf, so the SVG needs no font.
Colours are the panel's: ink #e9e9ea + accent #f0c330 (tool), ink #1a1a1a + accent #16804a (light).

    python scripts/make_logo.py        (needs: pip install fonttools)
"""
import re
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "panel" / "birdie-panel" / "html_ui" / "InGamePanels" / "Birdie" / "fonts" / "birdie-sans.ttf"
TEXT = "Bırdie"  # dotless i: the bird is its dot
SIZE = 72.0           # px per em
TRACK = 6.0           # letter spacing, px (the card: 3 px at 36 px)
PAD = 6.0


def build(ink, acc):
    f = TTFont(FONT)
    gs, cmap, hmtx = f.getGlyphSet(), f.getBestCmap(), f["hmtx"]
    k = SIZE / f["head"].unitsPerEm
    asc = f["hhea"].ascent * k
    x, paths, i_box = PAD, [], None
    for ch in TEXT:
        name = cmap[ord(ch)]
        pen = SVGPathPen(gs)
        gs[name].draw(TransformPen(pen, (k, 0, 0, -k, x, PAD + asc)))
        paths.append(re.sub(r"\d+\.\d{3,}", lambda m: f"{float(m.group()):.2f}", pen.getCommands()))
        adv = hmtx[name][0] * k
        if ch == "ı":
            i_box = (x, adv)
        x += adv + TRACK
    width = x - TRACK + PAD
    # x-height of the font: the bird sits above it, centred on the dotless i's stem
    xh = getattr(f["OS/2"], "sxHeight", 0) * k or 0.52 * SIZE
    cx = i_box[0] + i_box[1] / 2
    cy = PAD + asc - xh - 0.24 * SIZE
    r, wing, tail, sw = 0.082 * SIZE, 0.25 * SIZE, 0.19 * SIZE, 0.034 * SIZE
    bird = (f'<g fill="none" stroke="{acc}" stroke-width="{sw:.2f}" stroke-linecap="round">'
            f'<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{r:.2f}"/>'
            f'<path d="M{cx - r:.2f} {cy:.2f}H{cx - wing:.2f}M{cx + r:.2f} {cy:.2f}H{cx + wing:.2f}'
            f'M{cx:.2f} {cy - r:.2f}V{cy - tail:.2f}"/></g>')
    top = cy - tail - sw - 2
    height = PAD + asc + 0.02 * SIZE + PAD
    y0 = min(0.0, top)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 {y0:.1f} {width:.1f} {height - y0:.1f}" '
            f'width="{width:.0f}" height="{height - y0:.0f}" role="img" aria-label="Birdie">\n'
            f'  <path fill="{ink}" d="{" ".join(paths)}"/>\n  {bird}\n</svg>\n')


if __name__ == "__main__":
    for name, ink, acc in (("logo-dark.svg", "#e9e9ea", "#f0c330"), ("logo-light.svg", "#1a1a1a", "#16804a")):
        (ROOT / "res" / name).write_text(build(ink, acc), encoding="utf-8")
        print("res/" + name)
