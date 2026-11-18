"""Screenshot panel previews with headless Edge and lay them side by side.

  python tools/panel_preview/shoot.py out.png "birdie.html?en&still" "birdie.html?still&page=2" ... [--h 700] [--w 352]

Each argument is a page under the preview server (serve.py, port 8770). Captions = the arguments.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"


def shot(url, w, h, out, scale=2):
    """Headless windows are at least ~500 px wide: shoot wider, the page sits at the left (preview.js), crop."""
    subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--force-device-scale-factor={scale}",
                    f"--window-size={max(w, 600)},{h}", "--virtual-time-budget=6000", f"--screenshot={out}", url],
                   check=True, capture_output=True, timeout=60)
    im = Image.open(out)
    im.crop((0, 0, w * scale, im.height)).save(out)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("pages", nargs="+")
    ap.add_argument("--w", type=int, default=352)
    ap.add_argument("--h", type=int, default=700)
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--bg", default="#202020")
    a = ap.parse_args()
    ims = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, p in enumerate(a.pages):
            f = Path(tmp) / f"{i}.png"
            shot(f"http://127.0.0.1:{a.port}/{p}", a.w, a.h, f)
            ims.append(Image.open(f).convert("RGB"))
    pad, cap = 24, 44
    W = sum(im.width for im in ims) + pad * (len(ims) + 1)
    H = max(im.height for im in ims) + pad * 2 + cap
    sheet = Image.new("RGB", (W, H), a.bg)
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 22)
    except OSError:
        font = ImageFont.load_default()
    x = pad
    for p, im in zip(a.pages, ims):
        d.text((x, pad), p, fill="#9a9a9a", font=font)
        sheet.paste(im, (x, pad + cap))
        x += im.width + pad
    sheet.save(a.out)
    print(a.out, sheet.size)
