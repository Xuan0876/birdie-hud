"""Contact sheets of the HUD in different looks, drawn by the product's own code (birdie_shot) over a recorded frame
(tests/fixtures/<name>/frames, made by tests/make_golden.py, not in the repository) or a plain gradient.

  python tools/hud_shots.py out.png [--check 2] [--cols 3] [--scale 0.5] "caption|key=value key=value" ...
  python tools/hud_shots.py out.png --anim boot --steps 6 "boot|"     # one row of an opening

Needs build/border (or --exe) with birdie_shot built. Each cell = one argument: caption, "|", then birdie_shot
arguments (key=value, preset=..., anim=... p=...).
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FIX = ROOT / "tests" / "fixtures" / "lax_dusk_07L"


def shot(exe, check, args, out):
    r = subprocess.run([str(exe), str(FIX), str(check), str(out)] + args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"birdie_shot failed: {r.stdout} {r.stderr}")


def frame(check, box):
    x, y, w, h = box
    jpg = FIX / "frames" / f"{check:03d}.jpg"
    if not jpg.exists():  # the recording frames are not in the public repository: a plain dusk-sky gradient
        im = Image.new("RGBA", (w, h))
        px = im.load()
        for j in range(h):
            k = j / max(h - 1, 1)
            c = (int(40 + 30 * k), int(44 + 22 * k), int(62 + 6 * k), 255)
            for i in range(w):
                px[i, j] = c
        return im
    im = Image.open(jpg).convert("RGBA")
    return im.crop((x, y, x + w, y + h))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("cells", nargs="+")
    ap.add_argument("--check", type=int, default=2)
    ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--scale", type=float, default=0.5)
    ap.add_argument("--crop", default="", help="x0,y0,x1,y1 in overlay px (default: all)")
    ap.add_argument("--anim", default="")
    ap.add_argument("--steps", type=int, default=6)
    ap.add_argument("--exe", default=str(ROOT / "build" / "border" / "tests" / "parity" / "birdie_shot.exe"))
    a = ap.parse_args()
    import json
    meta = json.load(open(FIX / "fixture.json", encoding="utf-8"))
    box = meta["box_px"]
    cells = []
    for c in a.cells:
        cap, _, rest = c.partition("|")
        args = rest.split()
        if a.anim:
            for i in range(a.steps):
                p = (i + 1) / a.steps
                cells.append((f"{cap} p={p:.2f}", args + [f"anim={a.anim}", f"p={p}"]))
        else:
            cells.append((cap, args))
    bg = frame(a.check, box)
    crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else (0, 0, box[2], box[3])
    tiles = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (cap, args) in enumerate(cells):
            f = Path(tmp) / f"{i}.png"
            shot(a.exe, a.check, args, f)
            hud = Image.open(f).convert("RGBA")
            im = Image.alpha_composite(bg, hud).crop(crop)
            im = im.resize((int(im.width * a.scale), int(im.height * a.scale)), Image.LANCZOS)
            tiles.append((cap, im))
    cols = min(a.cols, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    tw, th = tiles[0][1].size
    cap_h, pad = 34, 10
    sheet = Image.new("RGB", (cols * (tw + pad) + pad, rows * (th + cap_h + pad) + pad), "#161616")
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 20)
    except OSError:
        font = ImageFont.load_default()
    for i, (cap, im) in enumerate(tiles):
        x = pad + (i % cols) * (tw + pad)
        y = pad + (i // cols) * (th + cap_h + pad)
        d.text((x + 2, y + 4), cap, fill="#bbbbbb", font=font)
        sheet.paste(im.convert("RGB"), (x, y + cap_h))
    sheet.save(a.out)
    print(a.out, sheet.size)


if __name__ == "__main__":
    main()
