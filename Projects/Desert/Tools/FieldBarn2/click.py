# click.py <x> <y> in shot-window 1400-wide scaled coords -> clicks the Editor window (content origin)
import sys, subprocess, json, time, ctypes, ctypes.util
js = r'''ObjC.import('CoreGraphics');
var l = $.CGWindowListCopyWindowInfo($.kCGWindowListOptionOnScreenOnly, 0);
var a = ObjC.deepUnwrap(ObjC.castRefToObject(l)); var r=[];
for (var w of a) if (w.kCGWindowOwnerName=='Editor') r.push(w.kCGWindowBounds);
JSON.stringify(r)'''
b = json.loads(subprocess.run(["osascript", "-l", "JavaScript", "-e", js], capture_output=True, text=True).stdout)
b = max(b, key=lambda w: w["Width"] * w["Height"])
import re
out = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-scratch/FIELD-BARN-2/win.png"], capture_output=True, text=True).stdout
IW, IH = [int(v) for v in re.findall(r": (\d+)", out)]
W = IW / 2.0
sx = b["Width"] / W
x = b["X"] + float(sys.argv[1]) * (IW / 1400) / 2.0 * sx
y = b["Y"] + (b["Height"] - IH / 2.0 * sx) + float(sys.argv[2]) * (IW / 1400) / 2.0 * sx
cg = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreGraphics"))
class P(ctypes.Structure): _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]
cg.CGEventCreateMouseEvent.restype = ctypes.c_void_p
cg.CGEventCreateMouseEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint32, P, ctypes.c_uint32]
cg.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
for t in (5, 1, 2):  # move, down, up
    e = cg.CGEventCreateMouseEvent(None, t, P(x, y), 0); cg.CGEventPost(0, e); time.sleep(0.6)
print("bounds", b, "click", x, y)
