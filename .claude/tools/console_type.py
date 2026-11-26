"""Windows: type text into another process's console input buffer (AttachConsole + WriteConsoleInput).

Not SendKeys: SendKeys goes to whatever window has the focus (a game, an editor). This reaches only the console of @p pid
and needs no focus.
"""
import ctypes, time
from ctypes import wintypes as w

k = ctypes.WinDLL("kernel32", use_last_error=True)


class KeyEvent(ctypes.Structure):
    _fields_ = [("down", w.BOOL), ("repeat", w.WORD), ("vk", w.WORD), ("scan", w.WORD), ("ch", w.WCHAR), ("ctrl", w.DWORD)]


class InputRecord(ctypes.Structure):
    _fields_ = [("type", w.WORD), ("key", KeyEvent)]


class ProcessEntry(ctypes.Structure):
    _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("pid", w.DWORD), ("heap", ctypes.c_size_t), ("module", w.DWORD),
                ("threads", w.DWORD), ("ppid", w.DWORD), ("prio", w.LONG), ("flags", w.DWORD), ("exe", w.WCHAR * 260)]


k.CreateToolhelp32Snapshot.restype = w.HANDLE
k.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
k.Process32FirstW.argtypes = k.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(ProcessEntry)]
k.CreateFileW.restype = w.HANDLE
k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
k.WriteConsoleInputW.argtypes = [w.HANDLE, ctypes.POINTER(InputRecord), w.DWORD, ctypes.POINTER(w.DWORD)]
k.CloseHandle.argtypes = [w.HANDLE]


def ancestor(pid, exe):
    """The nearest ancestor of @p pid whose image is @p exe, or None."""
    snap = k.CreateToolhelp32Snapshot(2, 0)
    e, table = ProcessEntry(), {}
    e.size = ctypes.sizeof(e)
    more = k.Process32FirstW(snap, ctypes.byref(e))
    while more:
        table[e.pid] = (e.ppid, e.exe.lower())
        more = k.Process32NextW(snap, ctypes.byref(e))
    k.CloseHandle(snap)
    for _ in range(16):
        if pid not in table:
            return None
        if table[pid][1] == exe:
            return pid
        pid = table[pid][0]
    return None


def type_into(pid, text, enters, wipe_lines):
    """Empty the prompt, type @p text, press Enter @p enters times. The caller has no console of its own."""
    k.FreeConsole()
    if not k.AttachConsole(pid):
        raise ctypes.WinError(ctypes.get_last_error())
    h = k.CreateFileW("CONIN$", 0xC0000000, 3, None, 3, 0, None)
    if h == w.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())

    def press(vk, ch, ctrl=0):
        recs = (InputRecord * 2)(*[InputRecord(1, KeyEvent(d, 1, vk, 0, ch, ctrl)) for d in (1, 0)])
        n = w.DWORD()
        if not k.WriteConsoleInputW(h, recs, 2, ctypes.byref(n)) or n.value != 2:
            raise ctypes.WinError(ctypes.get_last_error())
        time.sleep(0.01)

    try:
        for _ in range(wipe_lines):
            press(0x45, chr(5), 8)   # Ctrl+E
            press(0x55, chr(21), 8)  # Ctrl+U
            press(0x08, chr(8))      # Backspace
        time.sleep(0.2)
        for c in text:
            press(0, c)
        for _ in range(enters):
            time.sleep(0.7)
            press(0x0D, chr(13))
    finally:
        k.CloseHandle(h)
        k.FreeConsole()
