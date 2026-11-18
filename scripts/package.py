"""Make the release zip: dist/Birdie-<version>.zip, the same file for GitHub Releases and flightsim.to.

  scripts\\build.bat                     build first (release preset -> build/release)
  python scripts/package.py              -> dist/Birdie-0.9.1-beta.zip
  python scripts/package.py --build build/border --allow-dirty

Contents (one folder, so "extract here" does not scatter files):
  Birdie/Birdie.exe              carries the settings page and the in-sim panel (src/app/ingame.h)
  Birdie/SimConnect.dll          MSFS 2024 SDK, next to the exe (delay-loaded)
  Birdie/README.txt              install / use / in-sim panel / upgrade / uninstall / log, Chinese + English
  Birdie/Birdie-Manual-zh-CN.pdf the manual (scripts/make_manual.py --pdf, from docs/manual/)
  Birdie/Birdie-Manual-en.pdf
  Birdie/LICENSE.txt             GPL-3.0
  Birdie/THIRD_PARTY_NOTICES.md
  Birdie/licenses/               WebView2 SDK (BSD-3 + NOTICE), nlohmann/json (MIT), fonts in the panel (OFL)

Refuses to package a tree with uncommitted changes (the zip must match a commit, for the GPL source offer) unless
--allow-dirty, and an exe older than the newest source file.
"""
import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = "https://github.com/Xuan0876/birdie-hud"
FONTS = ROOT / "panel" / "birdie-panel" / "html_ui" / "InGamePanels" / "Birdie" / "fonts"


def version():
    cm = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(Birdie VERSION ([0-9][0-9.]*)", cm)
    if not m:
        sys.exit("no version in CMakeLists.txt")
    suf = re.search(r'set\(BIRDIE_VERSION_SUFFIX "([^"]*)"\)', cm)
    return m.group(1) + (suf.group(1) if suf else "")


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout.strip()


def newest_source():
    files = [p for d in ("src", "panel", "res") for p in (ROOT / d).rglob("*") if p.is_file()]
    files.append(ROOT / "CMakeLists.txt")
    return max(files, key=lambda p: p.stat().st_mtime)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build/release", help="build folder with Birdie.exe and SimConnect.dll")
    ap.add_argument("--allow-dirty", action="store_true", help="package uncommitted changes (test builds only)")
    a = ap.parse_args()

    ver = version()
    build = (ROOT / a.build).resolve()
    exe, dll = build / "Birdie.exe", build / "SimConnect.dll"
    for f in (exe, dll):
        if not f.exists():
            sys.exit(f"missing {f} (run scripts\\build.bat first)")
    newest = newest_source()
    if exe.stat().st_mtime < newest.stat().st_mtime:
        sys.exit(f"{exe} is older than {newest.relative_to(ROOT)}: rebuild first")
    manuals = [ROOT / "build" / "manual" / n for n in ("Birdie-Manual-zh-CN.pdf", "Birdie-Manual-en.pdf")]
    for f in manuals:
        if not f.exists():
            sys.exit(f"missing {f.relative_to(ROOT)} (run python scripts/make_manual.py --pdf first)")
    man_src = max((p for p in (ROOT / "docs" / "manual").rglob("*") if p.is_file()), key=lambda p: p.stat().st_mtime)
    if min(f.stat().st_mtime for f in manuals) < man_src.stat().st_mtime:
        sys.exit(f"manual PDFs are older than {man_src.relative_to(ROOT)}: run python scripts/make_manual.py --pdf")
    dirty = git("status", "--porcelain")
    if dirty and not a.allow_dirty:
        sys.exit("uncommitted changes (commit first, or --allow-dirty for a test zip):\n" + dirty)
    commit = git("rev-parse", "--short", "HEAD") + ("-dirty" if dirty else "")

    readme = (ROOT / "packaging" / "README.txt").read_text(encoding="utf-8").format(version=ver, repo=REPO)
    readme = readme.replace("\n", "\r\n")  # Notepad on older Windows

    files = [  # (path in the zip, source path or text)
        ("Birdie.exe", exe),
        ("SimConnect.dll", dll),
        ("README.txt", readme.encode("utf-8-sig")),
        *[(f.name, f) for f in manuals],
        ("LICENSE.txt", ROOT / "LICENSE"),
        ("THIRD_PARTY_NOTICES.md", ROOT / "THIRD_PARTY_NOTICES.md"),
        ("licenses/WebView2-LICENSE.txt", ROOT / "third_party" / "webview2" / "LICENSE.txt"),
        ("licenses/WebView2-NOTICE.txt", ROOT / "third_party" / "webview2" / "NOTICE.txt"),
        ("licenses/nlohmann-json-MIT.txt", ROOT / "third_party" / "nlohmann" / "LICENSE.MIT"),
    ]
    files += [(f"licenses/{p.name}", p) for p in sorted(FONTS.glob("OFL-*.txt"))]

    out = ROOT / "dist" / f"Birdie-{ver}.zip"
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, src in files:
            data = src if isinstance(src, bytes) else Path(src).read_bytes()
            z.writestr(f"Birdie/{name}", data)
        z.comment = f"Birdie {ver} ({commit}) {REPO}".encode("utf-8")
    print(f"{out.relative_to(ROOT)}  {out.stat().st_size // 1024} KB  Birdie {ver} ({commit})")
    for name, _ in files:
        print("  Birdie/" + name)


if __name__ == "__main__":
    main()
