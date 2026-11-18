"""Try to set the ChasePlane zoom by writing its L-var; prints the value read back after 1.5 s.

    python -I tools/zoom_calib/set_zoom.py 0.6
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "reference", "python"))
from simconnect_min import SimConnect  # noqa: E402

DLL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "release", "SimConnect.dll")


def read_zoom(sc):
    sc.request(2, 1, 1)  # once
    t0 = time.time()
    while time.time() - t0 < 2:
        for m in sc.poll():
            if m[0] == "data" and m[1] == 2:
                return struct.unpack_from("<d", m[3])[0]
            if m[0] == "exception":
                print("exception", m)
        time.sleep(0.02)
    return None


def main():
    target = float(sys.argv[1])
    sc = SimConnect("birdie_set_zoom", os.path.abspath(DLL))
    sc.add_var(1, "L:p42_cp_r_zoom", "number")
    print("before", read_zoom(sc))
    print("set ok", sc.set_data(1, [target]))
    time.sleep(1.5)
    print("after", read_zoom(sc))
    sc.close()


if __name__ == "__main__":
    main()
