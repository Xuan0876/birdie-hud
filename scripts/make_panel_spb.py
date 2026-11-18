"""Write the MSFS toolbar panel registration (InGamePanels/*.spb) for the Birdie community package.

The SDK builds .spb files with fspackagetool, which runs the simulator in a build mode. That cannot be done while
the user is flying, and it ties the panel to the SDK, so this script writes the small binary directly:

  header   magic + property table: one entry per property = GUID (16 bytes) + size (4 bytes, -1 = variable)
           (the table below is the one MSFS's builder emits for an InGamePanels document with one panel)
  body     u32 count (1) + u32 byte size, then records "u32 property index (1-based) + value";
           variable values are u32 length + bytes; a nested object is a variable value holding records and
           ending in a 0 index; strings are NUL-terminated and XORed with a fixed key stream.

Usage:
  python scripts/make_panel_spb.py <out.spb>          write Birdie's panel
  python scripts/make_panel_spb.py --check <a.spb>    decode a single-panel .spb and re-encode it byte for byte
                                                       (verified against installed MSFS 2024 panels, 11/15)
"""
import struct
import sys

# Header + property table, taken verbatim from a single-panel InGamePanels .spb built by the MSFS 2024 SDK.
HEADER = bytes.fromhex(
    "aceb140000000100000000000000000200000100000000000000120000000000000000000000000000000000000000000000"
    "7bd76feebb147c49ae590ccdd90eaa68ffffffff"  # 1  document
    "5e89acbdd05e0d488d09c4085785c2ffffffffff"  # 2  document type ("InGamePanels")
    "209482e0a24fb54c82b2f18b6d0f925608000000"  # 3  version (1, 0)
    "bafe14e7cabd184d865fee991266d625ffffffff"  # 4  file name
    "29cef350adaba54f83a2e6f24df78991ffffffff"  # 5  InGamePanelDefinition (nested)
    "c57fa5d3ffdec94eb84fdc9b29018ab4ffffffff"  # 6  id
    "a222fa5a9abede4b886da1a072879fdaffffffff"  # 7  Name
    "5f7db56813f5d14bb321615d0acf8d47ffffffff"  # 8  url
    "547fce57e6bb6f44bff865ce02b5300904000000"  # 9  resizeDirections (3 = both)
    "bcb73abae28f6b4d972931831d4252df04000000"  # 10 minWidth   (% of the screen)
    "3e4dcef03682b248a5b7559bec77810a04000000"  # 11 minHeight
    "d8b042325ba6b1469fcf9f4bdf367da604000000"  # 12 defaultWidth
    "317ea32f663dee44a95efcacc9aa93e504000000"  # 13 defaultHeight
    "e5e1e4e6551b8348b1ff14895c73c2ee04000000"  # 14 defaultTop
    "1f7f83448e01e04bb4167aa70d2cfb4b04000000"  # 15 defaultRight
    "d93bc7fbc791e84594443bc75da6651dffffffff"  # 16 icon
    "fe209e9d40ee8144b975076f58e8e14c04000000"  # 17 buttonVisible
)
KEY = bytes.fromhex("2a072b3132 5c63 8ebff152b5480c02540e566264b8c62183e7a46f901804a81cacc4c87591420bd34dde25300855385d8d95"
                    .replace(" ", ""))

BIRDIE = dict(
    file="birdie-panel.spb",
    id="PANEL_BIRDIE",
    name="Birdie",
    url="html_ui/InGamePanels/Birdie/birdie.html",
    resize=3,
    # Width and height are both % of the screen HEIGHT (ChasePlane registers 60 x 60, most panels 36 x 36; 11/17 the
    # user found 14.5 wide far too narrow on 2560x1440: ~209 px). 26 x 40 at 1440p = ~374 x 576 px: the 352 px design
    # plus the sim's title bar, the home page without scrolling.
    min_w=20.0, min_h=26.0,
    w=26.0, h=40.0,
    top=10.0, right=2.0,
    icon="ICON_TOOLBAR_BIRDIE",
    button=1,
)


def xstr(s):
    b = s.encode("ascii") + b"\0"
    if len(b) > len(KEY):
        raise ValueError(f"string longer than the known key ({len(KEY) - 1} chars): {s}")
    b = bytes(c ^ KEY[i] for i, c in enumerate(b))
    return struct.pack("<I", len(b)) + b


def rec(index, payload):
    return struct.pack("<I", index) + payload


def blob(records):
    data = b"".join(records) + struct.pack("<I", 0)
    return struct.pack("<I", len(data)) + data


def build(p):
    panel = [
        rec(6, xstr(p["id"])),
        rec(7, xstr(p["name"])),
        rec(8, xstr(p["url"])),
        rec(9, struct.pack("<I", p["resize"])),
        rec(10, struct.pack("<f", p["min_w"])),
        rec(11, struct.pack("<f", p["min_h"])),
        rec(12, struct.pack("<f", p["w"])),
        rec(13, struct.pack("<f", p["h"])),
        rec(14, struct.pack("<f", p["top"])),
        rec(15, struct.pack("<f", p["right"])),
        rec(16, xstr(p["icon"])),
        rec(17, struct.pack("<I", p["button"])),
    ]
    body = b"".join([
        rec(2, xstr("InGamePanels")),
        rec(3, struct.pack("<II", 1, 0)),
        rec(4, xstr(p["file"])),
        rec(5, blob(panel)),
    ]) + struct.pack("<I", 0)
    return HEADER + struct.pack("<II", 1, len(body)) + body


def decode(data):
    """The values of a single-panel .spb written with HEADER's property table."""
    if not data.startswith(HEADER):
        raise ValueError("different header / property table")
    o = len(HEADER) + 8
    out = {}
    names = {4: "file", 6: "id", 7: "name", 8: "url", 9: "resize", 10: "min_w", 11: "min_h", 12: "w", 13: "h",
             14: "top", 15: "right", 16: "icon", 17: "button"}

    def walk(o, end):
        while o < end:
            (i,) = struct.unpack_from("<I", data, o)
            o += 4
            if i == 0:
                continue
            if i in (1, 2, 4, 5, 6, 7, 8, 16):
                (n,) = struct.unpack_from("<I", data, o)
                v = data[o + 4:o + 4 + n]
                if i == 5:
                    walk(o + 4, o + 4 + n)
                elif i in names:
                    out[names[i]] = bytes(c ^ KEY[k] for k, c in enumerate(v))[:-1].decode("ascii")
                o += 4 + n
            elif i == 3:
                o += 8
            else:
                v = data[o:o + 4]
                out[names[i]] = struct.unpack("<I" if i in (9, 17) else "<f", v)[0]
                o += 4

    walk(o, len(data))
    return out


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        data = open(sys.argv[2], "rb").read()
        p = decode(data)
        print(p)
        same = build(p) == data
        print("re-encoded byte for byte:", same)
        sys.exit(0 if same else 1)
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    data = build(BIRDIE)
    assert decode(data) == {k: v for k, v in BIRDIE.items()}, "round trip"
    open(sys.argv[1], "wb").write(data)
    print(f"wrote {sys.argv[1]} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
