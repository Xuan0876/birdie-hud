"""ctypes wrapper for hudgfx.dll (native/hudgfx.cpp): GPU-drawn, click-through, topmost overlay window.

    g = HudGfx(x, y, w, h)          # physical screen pixels
    g.frame(cmds, text)             # float command list + string pool from hud.commands(); [] = clear
    g.move(x, y, w, h); g.show(True)
"""
import ctypes
import os
from array import array

HERE = os.path.dirname(os.path.abspath(__file__))


class HudGfx:
    def __init__(self, x, y, w, h, dll=None):
        self.dll = ctypes.CDLL(dll or os.path.join(HERE, "hudgfx.dll"))
        d = self.dll
        d.hg_init.argtypes = [ctypes.c_int] * 4
        d.hg_move.argtypes = [ctypes.c_int] * 4
        d.hg_show.argtypes = [ctypes.c_int]
        d.hg_frame.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p, ctypes.c_int]
        d.hg_error.restype = ctypes.c_wchar_p
        d.hg_set_family.argtypes = [ctypes.c_wchar_p]
        rc = d.hg_init(int(x), int(y), int(w), int(h))
        if rc:
            raise OSError(f"hudgfx 初始化失败 {rc}: {d.hg_error()}")
        self.box = (int(x), int(y), int(w), int(h))
        self.visible = False

    def set_family(self, fam):
        self.dll.hg_set_family(fam)

    def move(self, x, y, w, h):
        box = (int(x), int(y), int(w), int(h))
        if box != self.box:
            self.box = box
            self.dll.hg_move(*box)

    def show(self, on):
        if bool(on) != self.visible:
            self.visible = bool(on)
            self.dll.hg_show(1 if on else 0)

    def frame(self, cmds, text=""):
        buf = array("f", cmds)
        addr = buf.buffer_info()[0] if len(buf) else None
        rc = self.dll.hg_frame(addr, len(buf), text, len(text))
        return rc, (self.dll.hg_error() if rc else "")

    def render_to_memory(self, cmds, text, w, h):
        """Offline check: the same drawing into a w x h RGBA numpy array (premultiplied alpha)."""
        import numpy as np
        self.dll.hg_render_to_memory.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p, ctypes.c_int,
                                                 ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        buf = array("f", cmds)
        out = np.zeros((h, w, 4), np.uint8)
        rc = self.dll.hg_render_to_memory(buf.buffer_info()[0] if len(buf) else None, len(buf), text, len(text),
                                          w, h, out.ctypes.data)
        if rc:
            raise OSError(self.dll.hg_error())
        return out[:, :, [2, 1, 0, 3]]

    def close(self):
        self.dll.hg_shutdown()
