"""Minimal ctypes SimConnect client: data definitions, per-frame requests, system events, and (for the
playback rig) client events and SetDataOnSimObject.

Uses the SimConnect.dll from the MSFS 2024 SDK. Struct offsets follow SimConnect.h:
  SIMCONNECT_RECV                = dwSize, dwVersion, dwID                     (12 bytes)
  SIMCONNECT_RECV_EXCEPTION      = RECV + dwException, dwSendID, dwIndex
  SIMCONNECT_RECV_EVENT          = RECV + uGroupID, uEventID, dwData
  SIMCONNECT_RECV_SIMOBJECT_DATA = RECV + 7 DWORDs, data at offset 40
"""
import ctypes
import os
import struct
from ctypes import wintypes

DEFAULT_DLL = r"C:\MSFS 2024 SDK\SimConnect SDK\lib\SimConnect.dll"

RECV_ID_EXCEPTION = 1
RECV_ID_OPEN = 2
RECV_ID_QUIT = 3
RECV_ID_EVENT = 4
RECV_ID_SIMOBJECT_DATA = 8

DATATYPE_FLOAT64 = 4
DATATYPE_STRING256 = 9
PERIOD_NEVER = 0
PERIOD_ONCE = 1
PERIOD_SIM_FRAME = 3
OBJECT_ID_USER = 0
UNUSED = 0xFFFFFFFF
S_OK = 0


class SimConnectError(RuntimeError):
    pass


class SimConnect:
    def __init__(self, name="fpv_hud", dll_path=None):
        dll_path = dll_path or os.environ.get("SIMCONNECT_DLL", DEFAULT_DLL)
        self.dll = ctypes.WinDLL(dll_path)
        self._bind()
        self.h = wintypes.HANDLE()
        hr = self.dll.SimConnect_Open(ctypes.byref(self.h), name.encode(), None, 0, None, 0)
        if hr != S_OK:
            raise SimConnectError(f"SimConnect_Open failed (hr=0x{hr & 0xFFFFFFFF:08X}); is the sim running?")

    def _bind(self):
        d = self.dll
        d.SimConnect_Open.argtypes = [ctypes.POINTER(wintypes.HANDLE), ctypes.c_char_p, wintypes.HWND,
                                      wintypes.DWORD, wintypes.HANDLE, wintypes.DWORD]
        d.SimConnect_Close.argtypes = [wintypes.HANDLE]
        d.SimConnect_AddToDataDefinition.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_char_p,
                                                     ctypes.c_char_p, ctypes.c_int, ctypes.c_float, wintypes.DWORD]
        d.SimConnect_ClearDataDefinition.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        d.SimConnect_RequestDataOnSimObject.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                                        wintypes.DWORD, ctypes.c_int, wintypes.DWORD,
                                                        wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
        d.SimConnect_SubscribeToSystemEvent.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_char_p]
        d.SimConnect_GetNextDispatch.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p),
                                                 ctypes.POINTER(wintypes.DWORD)]
        d.SimConnect_GetLastSentPacketID.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        d.SimConnect_CameraSetRelative6DOF.argtypes = [wintypes.HANDLE] + [ctypes.c_float] * 6
        d.SimConnect_CameraSetRelative6DOF.restype = ctypes.c_long
        d.SimConnect_MapClientEventToSimEvent.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_char_p]
        d.SimConnect_TransmitClientEvent.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                                     wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
        d.SimConnect_SetDataOnSimObject.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                                    wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        for f in ("SimConnect_MapClientEventToSimEvent", "SimConnect_TransmitClientEvent",
                  "SimConnect_SetDataOnSimObject"):
            getattr(d, f).restype = ctypes.c_long
        for f in ("SimConnect_Open", "SimConnect_Close", "SimConnect_AddToDataDefinition",
                  "SimConnect_ClearDataDefinition", "SimConnect_RequestDataOnSimObject",
                  "SimConnect_SubscribeToSystemEvent", "SimConnect_GetNextDispatch",
                  "SimConnect_GetLastSentPacketID"):
            getattr(d, f).restype = ctypes.c_long

    def last_packet_id(self):
        pid = wintypes.DWORD()
        self.dll.SimConnect_GetLastSentPacketID(self.h, ctypes.byref(pid))
        return pid.value

    def add_var(self, define_id, name, unit, datatype=DATATYPE_FLOAT64):
        """Adds a datum; returns the packet id so a later exception can be traced to this var."""
        unit_arg = unit.encode() if unit else None
        hr = self.dll.SimConnect_AddToDataDefinition(self.h, define_id, name.encode(), unit_arg,
                                                     datatype, 0.0, UNUSED)
        if hr != S_OK:
            raise SimConnectError(f"AddToDataDefinition({name}) hr=0x{hr & 0xFFFFFFFF:08X}")
        return self.last_packet_id()

    def clear_definition(self, define_id):
        self.dll.SimConnect_ClearDataDefinition(self.h, define_id)

    def request(self, request_id, define_id, period=PERIOD_SIM_FRAME):
        hr = self.dll.SimConnect_RequestDataOnSimObject(self.h, request_id, define_id, OBJECT_ID_USER,
                                                        period, 0, 0, 0, 0)
        if hr != S_OK:
            raise SimConnectError(f"RequestDataOnSimObject hr=0x{hr & 0xFFFFFFFF:08X}")
        return self.last_packet_id()

    def camera_set_relative_6dof(self, dx, dy, dz, pitch, bank, heading):
        """Moves the cockpit camera relative to the eyepoint (metres / degrees). Legacy API; only cockpit views."""
        hr = self.dll.SimConnect_CameraSetRelative6DOF(self.h, dx, dy, dz, pitch, bank, heading)
        return hr == S_OK, self.last_packet_id()

    def map_event(self, event_id, name):
        self.dll.SimConnect_MapClientEventToSimEvent(self.h, event_id, name.encode())
        return self.last_packet_id()

    def transmit(self, event_id, data=0):
        """Fires a mapped key event on the user aircraft (highest priority group)."""
        hr = self.dll.SimConnect_TransmitClientEvent(self.h, OBJECT_ID_USER, event_id, data & 0xFFFFFFFF, 1, 0x10)
        return hr == S_OK

    def set_data(self, define_id, values):
        """Writes a FLOAT64-only data definition on the user aircraft."""
        buf = struct.pack(f"<{len(values)}d", *values)
        hr = self.dll.SimConnect_SetDataOnSimObject(self.h, define_id, OBJECT_ID_USER, 0, 0, len(buf), buf)
        return hr == S_OK

    def subscribe(self, event_id, name):
        self.dll.SimConnect_SubscribeToSystemEvent(self.h, event_id, name.encode())

    def poll(self):
        """Yields every pending message as a tuple: ('data', request_id, count, payload_bytes) / ('event', event_id, data)
        / ('exception', code, send_id, index) / ('open',) / ('quit',)."""
        ptr = ctypes.c_void_p()
        size = wintypes.DWORD()
        while self.dll.SimConnect_GetNextDispatch(self.h, ctypes.byref(ptr), ctypes.byref(size)) == S_OK:
            raw = ctypes.string_at(ptr.value, size.value)
            _, _, rid = struct.unpack_from("<III", raw, 0)
            if rid == RECV_ID_SIMOBJECT_DATA:
                request_id, _obj, _def, _flags, _n, _of, count = struct.unpack_from("<7I", raw, 12)
                yield ("data", request_id, count, raw[40:])
            elif rid == RECV_ID_EVENT:
                _group, event_id, data = struct.unpack_from("<III", raw, 12)
                yield ("event", event_id, data)
            elif rid == RECV_ID_EXCEPTION:
                code, send_id, index = struct.unpack_from("<III", raw, 12)
                yield ("exception", code, send_id, index)
            elif rid == RECV_ID_OPEN:
                yield ("open",)
            elif rid == RECV_ID_QUIT:
                yield ("quit",)

    def close(self):
        if self.h:
            self.dll.SimConnect_Close(self.h)
            self.h = None
