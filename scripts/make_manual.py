"""Builds the user manual: pictures into docs/manual/img/, PDFs into build/manual/.

  python scripts/make_manual.py            pictures + PDFs
  python scripts/make_manual.py --pdf      PDFs only (pictures as committed)

Pictures are drawn by the product itself: the HUD by birdie_shot (tests/parity) over a plain dusk gradient, the
settings window by headless Edge from the real panel page with the preview's fake Birdie (tools/panel_preview). No
simulator pictures. PDFs: docs/manual/manual.<lang>.html printed by headless Edge.
Needs: a build with birdie_shot (--exe), Microsoft Edge, Pillow.
"""
import argparse
import http.server
import functools
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FIX = ROOT / "tests" / "fixtures" / "lax_dusk_07L"
MAN = ROOT / "docs" / "manual"
IMG = MAN / "img"
OUT = ROOT / "build" / "manual"
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
# Headless Edge can return before it has written its output (with another Edge open on the same profile it hands the
# job over and exits), and a shared profile can serve cached panel scripts. So: a profile of its own for this run,
# the old output deleted first, and a wait until the new file is there and has stopped growing.
EDGE_PROFILE = tempfile.mkdtemp(prefix="birdie-manual-edge-")


def edge(args, out, timeout):
    out = Path(out)
    out.unlink(missing_ok=True)
    subprocess.run([EDGE, "--headless=new", f"--user-data-dir={EDGE_PROFILE}", "--disable-gpu", *args],
                   check=True, capture_output=True, timeout=timeout)
    deadline, last = time.time() + timeout, -1
    while time.time() < deadline:
        size = out.stat().st_size if out.exists() else -1
        if size > 0 and size == last:
            return
        last = size
        time.sleep(0.5)
    sys.exit(f"Edge did not write {out}")
LANGS = ("zh-CN", "en")

# the HUD pictures: (name, fixture check, birdie_shot arguments, crop in overlay px x0, y0, x1, y1)
HUD_CROP = (590, 160, 1500, 820)
WIDE = (560, 30, 1530, 900)
HUD_SHOTS = [
    ("hud", 4, ["hud_scale=1.0", "show_tdz=true"], HUD_CROP),
    ("preset-airbus", 3, ["hud_scale=1.0", "show_tdz=true", "preset=airbus"], WIDE),
    ("preset-boeing", 3, ["hud_scale=1.0", "show_tdz=true", "preset=boeing"], WIDE),
    ("preset-fighter", 3, ["hud_scale=1.0", "show_tdz=true", "preset=fighter"], WIDE),
    ("preset-minimal", 3, ["hud_scale=1.0", "show_tdz=true", "preset=minimal"], WIDE),
    ("crosswind", 3, ["hud_scale=1.0", "show_tdz=true", "xwind_mode=true"], WIDE),
]
# numbered callouts on hud.png: (number, circle x, y, points at x, y) in the cropped picture's px, placed by eye.
# The manual's legend uses the same numbers.
CALLOUTS = [
    (1, 560, 385, 462, 326), (2, 590, 278, 514, 312), (3, 326, 280, 400, 312), (4, 454, 405, 454, 352),
    (5, 650, 365, 650, 329), (6, 668, 478, 612, 458), (7, 535, 236, 484, 318), (8, 300, 222, 300, 262),
    (9, 680, 222, 645, 268), (10, 250, 430, 322, 398), (11, 380, 585, 432, 546), (12, 515, 176, 470, 196),
    (13, 600, 50, 525, 84), (14, 58, 360, 128, 330), (15, 38, 130, 84, 166), (16, 672, 388, 728, 372),
    (17, 800, 462, 748, 450), (18, 858, 322, 802, 340),
]


def gradient(w, h):
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for j in range(h):
        k = j / max(h - 1, 1)
        c = (int(34 + 30 * k), int(38 + 22 * k), int(54 + 8 * k), 255)
        for i in range(w):
            px[i, j] = c
    return im


def hud_picture(exe, check, args, crop, out):
    import json
    box = json.load(open(FIX / "fixture.json", encoding="utf-8"))["box_px"]
    with tempfile.TemporaryDirectory() as td:
        png = Path(td) / "hud.png"
        r = subprocess.run([str(exe), str(FIX), str(check), str(png)] + args, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"birdie_shot failed: {r.stdout} {r.stderr}")
        hud = Image.open(png).convert("RGBA")
    bg = gradient(box[2], box[3])
    im = Image.alpha_composite(bg, hud).crop(crop)
    im.convert("RGB").save(out, optimize=True)
    return im


def callouts(path, marks):
    im = Image.open(path).convert("RGBA")
    d = ImageDraw.Draw(im)
    font = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 20)
    for n, x, y, tx, ty in marks:
        r = 15
        d.line((x, y, tx, ty), fill=(250, 250, 250, 200), width=2)
        d.ellipse((tx - 3, ty - 3, tx + 3, ty + 3), fill=(250, 250, 250, 230))
        d.ellipse((x - r, y - r, x + r, y + r), fill=(250, 250, 250, 235), outline=(20, 20, 22, 255), width=2)
        d.text((x, y + 1), str(n), font=font, fill=(20, 20, 22, 255), anchor="mm")
    im.convert("RGB").save(path, optimize=True)


# the settings window: (name, query) on the preview server; one picture per language
PANEL_SHOTS = [
    ("panel-home", "still&why=taxi"),
    ("panel-symbols", "still&page=1"),
    ("panel-fine", "still&page=2"),
    ("panel-look", "still&page=5"),
    ("panel-approach", "still&page=8"),
    ("panel-welcome", "still&welcome"),
]
PANEL_W, PANEL_BG = 352, (32, 32, 32)


class PreviewServer:
    """tools/panel_preview/serve.py in a thread, on a free port."""

    def __enter__(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("serve", ROOT / "tools" / "panel_preview" / "serve.py")
        serve = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(serve)
        self.httpd = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), functools.partial(serve.Handler, directory=str(serve.PAGE)))
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()


def panel_picture(port, query, out, scale=2):
    """Headless Edge, tall window; the page sits at the left (preview.js): crop its width, trim the empty bottom."""
    edge(["--hide-scrollbars", f"--force-device-scale-factor={scale}", "--window-size=600,1400",
          "--virtual-time-budget=8000", f"--screenshot={out}", f"http://127.0.0.1:{port}/birdie.html?{query}"], out, 90)
    im = Image.open(out).convert("RGB")
    im = im.crop((0, 0, PANEL_W * scale, im.height))
    px, bottom = im.load(), 0
    for y in range(im.height - 1, -1, -1):
        if any(sum(abs(a - b) for a, b in zip(px[x, y], PANEL_BG)) > 12 for x in range(0, im.width, 4)):
            bottom = y + 1
            break
    im.crop((0, 0, im.width, max(bottom, 1))).save(out, optimize=True)


PDF_NAMES = {"zh-CN": "Birdie-Manual-zh-CN.pdf", "en": "Birdie-Manual-en.pdf"}


def version():
    import re
    cm = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    v = re.search(r"project\(Birdie VERSION ([0-9][0-9.]*)", cm).group(1)
    suf = re.search(r'set\(BIRDIE_VERSION_SUFFIX "([^"]*)"\)', cm)
    return v + (suf.group(1) if suf else "")


def pdfs():
    import datetime
    OUT.mkdir(parents=True, exist_ok=True)
    ver, day = version(), datetime.date.today().isoformat()
    for lang in LANGS:
        src = (MAN / f"manual.{lang}.html").read_text(encoding="utf-8")
        tmp = MAN / f".print.{lang}.html"  # next to the source, so img/ and ../../res/ resolve
        tmp.write_text(src.replace("{{version}}", ver).replace("{{date}}", day), encoding="utf-8")
        out = OUT / PDF_NAMES[lang]
        try:
            edge(["--no-pdf-header-footer", "--virtual-time-budget=8000", f"--print-to-pdf={out}", tmp.as_uri()],
                 out, 120)
        finally:
            tmp.unlink(missing_ok=True)
        print(out.relative_to(ROOT), f"{out.stat().st_size // 1024} KB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "build" / "release" / "tests" / "parity" / "birdie_shot.exe"))
    ap.add_argument("--pdf", action="store_true", help="PDFs only")
    ap.add_argument("--only", default="", help="comma-separated picture names (debugging)")
    a = ap.parse_args()
    IMG.mkdir(parents=True, exist_ok=True)
    if not a.pdf:
        only = set(filter(None, a.only.split(",")))
        for name, check, args, crop in HUD_SHOTS:
            if only and name not in only:
                continue
            out = IMG / f"{name}.png"
            hud_picture(a.exe, check, args, crop, out)
            if name == "hud" and CALLOUTS:
                callouts(out, CALLOUTS)
            print(out.relative_to(ROOT))
        with PreviewServer() as srv:
            for name, query in PANEL_SHOTS:
                if only and name not in only:
                    continue
                for lang in LANGS:
                    out = IMG / f"{name}.{lang}.png"
                    panel_picture(srv.port, query + ("&en" if lang == "en" else ""), out)
                    print(out.relative_to(ROOT))
        if only:
            return
    pdfs()


if __name__ == "__main__":
    main()
