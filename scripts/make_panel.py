"""Build the generated parts of the Birdie community package (panel/birdie-panel), then (optionally) install it.

  python scripts/make_panel.py            spb + font subsets + layout.json
  python scripts/make_panel.py --install  ... and copy the package into the MSFS 2024 Community folder

The package is also built into Birdie.exe (CMakeLists: panel_pack.rc, src/app/ingame.h): rebuild after running this.
Users install it from Birdie's settings; --install here is a developer shortcut.

Generated (committed, so the package folder is complete as it is):
  InGamePanels/birdie-panel.spb                registration of the toolbar panel (scripts/make_panel_spb.py)
  html_ui/InGamePanels/Birdie/fonts/*.ttf      subsets of OFL fonts with only the characters the panel can show:
                                               the sim's Coherent GT has no system font fallback, and full CJK fonts
                                               are 20+ MB. Sources are downloaded once into build/font-src/.
  layout.json                                  every file with its size (MSFS reads the package through it)

Needs Python 3 with fontTools (pip install fonttools).
"""
import json
import os
import re
import shutil
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PKG = ROOT / "panel" / "birdie-panel"
PAGE = PKG / "html_ui" / "InGamePanels" / "Birdie"
FONTS = PAGE / "fonts"
SRC = ROOT / "build" / "font-src"
OFL = "https://raw.githubusercontent.com/google/fonts/main/ofl/"
SOURCES = {
    "NotoSerifSC[wght].ttf": "notoserifsc/NotoSerifSC%5Bwght%5D.ttf",
    "NotoSansSC[wght].ttf": "notosanssc/NotoSansSC%5Bwght%5D.ttf",
    "CormorantGaramond-Italic[wght].ttf": "cormorantgaramond/CormorantGaramond-Italic%5Bwght%5D.ttf",
    "CormorantGaramond[wght].ttf": "cormorantgaramond/CormorantGaramond%5Bwght%5D.ttf",
    "Jost[wght].ttf": "jost/Jost%5Bwght%5D.ttf",
    "BarlowSemiCondensed-Medium.ttf": "barlowsemicondensed/BarlowSemiCondensed-Medium.ttf",
    "OFL-noto.txt": "notoserifsc/OFL.txt",
    "OFL-cormorant.txt": "cormorantgaramond/OFL.txt",
    "OFL-jost.txt": "jost/OFL.txt",
    "OFL-barlow.txt": "barlowsemicondensed/OFL.txt",
}
# output name, source, weight (None = static font), which characters, Latin partner (source, weight, scale)
# The partner's Latin glyphs replace the Noto ones (Coherent GT has no per-glyph font fallback, so the panel's
# English faces have to live in the same file as the Chinese). Vertical metrics stay Noto's, so Chinese layout
# does not move. Cormorant is drawn 1.12x: at the same size it looks much smaller than Noto / Jost.
OUTPUTS = [
    ("birdie-serif.ttf", "NotoSerifSC[wght].ttf", 400, "text", ("CormorantGaramond[wght].ttf", 500, 1.12)),
    ("birdie-sans-light.ttf", "NotoSansSC[wght].ttf", 300, "text", ("Jost[wght].ttf", 300, 1)),
    ("birdie-sans.ttf", "NotoSansSC[wght].ttf", 400, "text", ("Jost[wght].ttf", 400, 1)),
    ("birdie-numerals.ttf", "CormorantGaramond-Italic[wght].ttf", 400, "latin", None),
    ("birdie-hud.ttf", "BarlowSemiCondensed-Medium.ttf", None, "latin", None),
]
LATIN = "".join(chr(c) for c in range(0x20, 0x7F)) + "°±−–—·’‘“”…×éı"  # ı: the wordmark's i, dotted with the bird


def fetch_sources():
    SRC.mkdir(parents=True, exist_ok=True)
    for name, path in SOURCES.items():
        if not (SRC / name).exists():
            print("downloading", name)
            urllib.request.urlretrieve(OFL + path, SRC / name)


def panel_text():
    """Every character the panel can show: its own words and the strings Birdie sends (RA notes, notices)."""
    text = LATIN + "，。、：；？！（）《》「」【】"
    for f in [PAGE / "birdie.js", PAGE / "birdie.html", ROOT / "src" / "app" / "i18n.cpp",
              ROOT / "src" / "app" / "app.cpp", ROOT / "src" / "terrain" / "store.cpp"]:  # region names
        text += f.read_text(encoding="utf-8")
    # \uXXXX escapes in the JS
    text += "".join(chr(int(h, 16)) for h in re.findall(r"\\u([0-9a-fA-F]{4})", text))
    return "".join(sorted(set(c for c in text if c >= " ")))


def subset_font(src, wght, text, scale=1):
    from fontTools import subset
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.scaleUpem import scale_upem
    from fontTools.varLib import instancer

    font = TTFont(SRC / src)
    if wght is not None and "fvar" in font:
        font = instancer.instantiateVariableFont(font, {"wght": wght})
    if scale != 1:  # every coordinate (outlines, advances, kerning) x scale, then claim the old em: glyphs grow
        upem = font["head"].unitsPerEm
        scale_upem(font, round(upem * scale))
        font["head"].unitsPerEm = upem
    opts = subset.Options()
    opts.hinting = False
    opts.desubroutinize = True
    opts.layout_features = ["kern", "liga", "locl", "vert", "halt", "palt"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.drop_tables += ["vhea", "vmtx", "VORG", "DSIG", "STAT", "MVAR", "HVAR", "gasp"]
    s = subset.Subsetter(opts)
    s.populate(text=text)
    s.subset(font)
    return font


def merge_latin(cjk, latin, out):
    """Noto subset + partner Latin subset -> one font; Noto's names and vertical metrics."""
    import tempfile
    from fontTools import merge

    with tempfile.TemporaryDirectory() as tmp:
        a, b = Path(tmp) / "cjk.ttf", Path(tmp) / "latin.ttf"
        cjk.save(a)
        latin.save(b)
        merged = merge.Merger(options=merge.Options(drop_tables=["vhea", "vmtx"])).merge([str(a), str(b)])
    for table, fields in [("hhea", ["ascent", "descent", "lineGap"]),
                          ("OS/2", ["sTypoAscender", "sTypoDescender", "sTypoLineGap", "usWinAscent", "usWinDescent"])]:
        for f in fields:
            setattr(merged[table], f, getattr(cjk[table], f))
    merged.save(out)


def make_fonts():
    from fontTools.ttLib import TTFont

    FONTS.mkdir(parents=True, exist_ok=True)
    chars = {"text": panel_text(), "latin": LATIN}
    for out, src, wght, which, partner in OUTPUTS:
        text = chars[which]
        if partner:
            psrc, pwght, pscale = partner
            have = set(TTFont(SRC / psrc)["cmap"].getBestCmap())
            latin = "".join(c for c in text if ord(c) in have and (c < "⺀"))  # never take CJK punctuation
            latin_font = subset_font(psrc, pwght, latin, pscale)
            cjk_font = subset_font(src, wght, "".join(c for c in text if c not in latin))
            merge_latin(cjk_font, latin_font, FONTS / out)
            note = f" ({len(latin)} from {psrc.split('[')[0]})"
        else:
            subset_font(src, wght, text).save(FONTS / out)
            note = ""
        print(f"{out}: {len(text)} characters{note}, {(FONTS / out).stat().st_size // 1024} KB")
    for lic in ["OFL-noto.txt", "OFL-cormorant.txt", "OFL-jost.txt", "OFL-barlow.txt"]:
        shutil.copyfile(SRC / lic, FONTS / lic)


def make_spb():
    sys.path.insert(0, str(Path(__file__).parent))
    import make_panel_spb as m
    out = PKG / "InGamePanels" / "birdie-panel.spb"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(m.build(m.BIRDIE))
    print("birdie-panel.spb")


def make_layout():
    files = []
    for p in sorted(PKG.rglob("*")):
        rel = p.relative_to(PKG).as_posix()
        if p.is_file() and rel not in ("layout.json", "manifest.json"):
            files.append({"path": rel, "size": p.stat().st_size, "date": 0})
    (PKG / "layout.json").write_text(json.dumps({"content": files}, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"layout.json: {len(files)} files")


def community_folder():
    """Community folder of MSFS 2024 (Microsoft Store or Steam), from UserCfg.opt."""
    local, roaming = os.environ.get("LOCALAPPDATA", ""), os.environ.get("APPDATA", "")
    for cfg in [Path(local) / "Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/UserCfg.opt",
                Path(roaming) / "Microsoft Flight Simulator 2024/UserCfg.opt"]:
        if cfg.exists():
            m = re.search(r'InstalledPackagesPath\s+"([^"]+)"', cfg.read_text(encoding="utf-8", errors="replace"))
            if m and (Path(m.group(1)) / "Community").is_dir():
                return Path(m.group(1)) / "Community"
    return None


def install():
    dest_root = community_folder()
    if not dest_root:
        sys.exit("MSFS 2024 Community folder not found (UserCfg.opt). Copy panel/birdie-panel there by hand.")
    dest = dest_root / "birdie-panel"
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(PKG, dest)
    print("installed to", dest)
    print("Restart MSFS once; then the bird is in the toolbar (top of the screen).")


if __name__ == "__main__":
    fetch_sources()
    make_spb()
    make_fonts()
    make_layout()
    if "--install" in sys.argv:
        install()
