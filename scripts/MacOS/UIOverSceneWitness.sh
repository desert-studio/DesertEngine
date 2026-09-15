#!/usr/bin/env bash
# UIOverSceneWitness.sh — is the UI canvas drawn OVER the 3D, and does a VIEW CHANGE end it? (Ю18)
#
#     scripts/MacOS/UIOverSceneWitness.sh [B-scene.desce] [Debug|Release]
#     scripts/MacOS/UIOverSceneWitness.sh Clouds_Sunset.desce
#
# WHY A SCRIPT AND NOT A SUITE. Half of "over 3D" is a position in a Vulkan command buffer and is
# asserted on integers by Desert/Tests/Engine/UICanvasOverScene, which CI can run. The other half is the
# picture, and it needs a live editor, a window and a device — so it lives here, and it is the half that
# would catch a canvas that is ordered correctly and still not on screen.
#
# WHAT IT MEASURES, AND WHY NOT THE WHOLE FRAME. `UI_OverScene.desce` carries a MARKER panel: opaque,
# saturated, at a fixed place on the canvas, so every pixel inside it is 100% canvas and 0% scene. The
# witness is "the marker is found, at the same box, before and after the round trip". That separation is
# not fastidiousness — the 3D half of this frame DOES move across a scene round trip (a foreign scene's
# environment is inherited on the visit right after it; measured at 81.4% of pixels, filed separately),
# and a witness that gated on the whole frame would be red for somebody else's defect forever.
#
# THE PROTOCOL IS A->B->A because A->A cannot see the class of defect this is about: reopening the same
# scene twice is byte-identical here (measured: 0 of 560 560), and only a visit to a FOREIGN scene moves
# anything. Both runs are printed, so the control is in the output beside the result.
#
# Exit status: 0 the canvas survived, 1 it did not, 2 the harness could not run.
set -uo pipefail

B_SCENE="${1:-Clouds_Sunset.desce}"
CONFIG="${2:-Debug}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EDITOR_BIN="$ROOT/build/Bin/$CONFIG/Editor"
CTL="$ROOT/build/Bin/$CONFIG/DesertCtl"
DIFF="$ROOT/build/Bin/$CONFIG/ImageDiff"

for binary in "$EDITOR_BIN" "$CTL" "$DIFF"; do
    if [ ! -x "$binary" ]; then
        echo "$0: $binary is not there. Build it: make Editor config=$(echo "$CONFIG" | tr 'A-Z' 'a-z') -j8" >&2
        echo "$0: ImageDiff and DesertCtl are TOOLS — the test sweep skips everything in ls Tools, so a" >&2
        echo "  stale one is possible and is worth rebuilding before believing this script." >&2
        exit 2
    fi
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/desert-uiover-XXXXXX")"
SOCKET="$WORK/ctl.sock"
LOG="$WORK/editor.log"

# THE EXPORTS HAPPEN HERE AND NOT THROUGH `env`. macOS SIP strips DYLD_* when a protected binary is
# exec'd, so `env VAR=... nohup Editor` loses DYLD_FALLBACK_LIBRARY_PATH and the editor dies at
# glfwVulkanSupported() — which reads as a broken build rather than a missing variable. Measured twice.
B="$(brew --prefix)"
export VK_ICD_FILENAMES="$B/etc/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$B/share/vulkan/explicit_layer.d"
export DYLD_FALLBACK_LIBRARY_PATH="$B/lib"
# ~/.desertengine/editor.json IS THE OWNER'S LIVE SETTINGS and the editor writes it on exit. A witness
# that changes the machine it runs on is not a witness.
export HOME="$WORK/home"
mkdir -p "$HOME"

( cd "$ROOT/Editor" && "$EDITOR_BIN" --project Desert.deproj --control-socket "$SOCKET" ) > "$LOG" 2>&1 &

cleanup() {
    "$CTL" --socket "$SOCKET" quit 0 > /dev/null 2>&1
    # The editor segfaults during teardown (a known shutdown bug), so its status says nothing and is
    # deliberately not read. Every PNG was written long before this point.
}
trap cleanup EXIT

open_scene() { "$CTL" --socket "$SOCKET" --wait 60 run Scene "Open Scene $1" > /dev/null; }
capture()    { "$CTL" --socket "$SOCKET" --wait 60 shot-viewport "$WORK/$1.png" > /dev/null; }

step() { printf '  %-28s %s\n' "$1" "$2"; }

echo "witness: canvas over 3D across A -> B -> A   (B = $B_SCENE)"
echo "  work dir: $WORK"

open_scene UI_OverScene.desce || { echo "could not open UI_OverScene.desce; see $LOG" >&2; exit 2; }
capture A1                    || { echo "could not capture; see $LOG" >&2; exit 2; }
open_scene "$B_SCENE"         || { echo "could not open $B_SCENE; see $LOG" >&2; exit 2; }
capture B                     || exit 2
open_scene UI_OverScene.desce || exit 2
capture A2                    || exit 2
# The CONTROL: the same scene reopened with nothing foreign in between.
open_scene UI_OverScene.desce || exit 2
capture A3                    || exit 2

python3 - "$WORK" "$DIFF" <<'PY'
import subprocess, sys, zlib, struct

WORK, DIFF = sys.argv[1], sys.argv[2]

# MEASURED, NOT PREDICTED. The marker is authored [0, 0.85, 1.0] in linear space and reaches the screen
# as (82, 197, 204) — the tonemapper lifts red by a third of the range. Predicting it would have made the
# witness blind; the measured triple is the constant, and a colour that drifts away from it shows up as
# "MARKER NOT FOUND" rather than as a quiet pass.
TARGET, TOL = (82, 197, 204), 8
EXPECT_PX = 6200   # the marker's area at the reference viewport, +-2%

def decode(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path
    i, idat = 8, b''
    while i < len(d):
        ln, typ = struct.unpack('>I4s', d[i:i+8]); body = d[i+8:i+8+ln]
        if typ == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', body[:10]); assert depth == 8
            ch = {0:1, 2:3, 3:1, 4:2, 6:4}[color]
        elif typ == b'IDAT': idat += body
        elif typ == b'IEND': break
        i += 12 + ln
    raw, stride = zlib.decompress(idat), w * ch
    out, prev, pos = bytearray(stride*h), bytearray(stride), 0
    for y in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos+stride]); pos += stride
        if f == 1:
            for x in range(ch, stride): line[x] = (line[x] + line[x-ch]) & 255
        elif f == 2:
            for x in range(stride): line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x-ch] if x >= ch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x-ch] if x >= ch else 0; b = prev[x]
                c = prev[x-ch] if x >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                line[x] = (line[x] + (a if (pa <= pb and pa <= pc) else (b if pb <= pc else c))) & 255
        out[y*stride:(y+1)*stride] = line; prev = line
    return w, h, ch, bytes(out)

def find_marker(path):
    w, h, ch, px = decode(path)
    xs, ys, n = [], [], 0
    for y in range(h):
        base = y * w * ch
        for x in range(w):
            i = base + x * ch
            if all(abs(px[i+k] - TARGET[k]) <= TOL for k in range(3)):
                xs.append(x); ys.append(y); n += 1
    if not xs: return None
    return (min(xs), min(ys), max(xs)+1, max(ys)+1, n, w, h)

def line(tag, v): print(f"  {tag:<28} {v}")

res = {t: find_marker(f"{WORK}/{t}.png") for t in ('A1', 'B', 'A2', 'A3')}
fail = []

for t in ('A1', 'A2', 'A3'):
    r = res[t]
    if r is None:
        line(f"{t}: marker", "NOT FOUND  <-- the canvas is not on top of the 3D"); fail.append(t)
    else:
        line(f"{t}: marker", f"box=({r[0]},{r[1]})-({r[2]},{r[3]}) px={r[4]} frame={r[5]}x{r[6]}")
        if abs(r[4] - EXPECT_PX) > EXPECT_PX * 0.02:
            line(f"{t}: marker area", f"{r[4]} px, expected ~{EXPECT_PX}  <-- partly occluded?"); fail.append(t)

# The negative control of the instrument itself: B has no canvas, so the marker MUST be absent. Without
# this the three lines above would pass on a locator that answers yes to anything.
if res['B'] is None:
    line("B: marker", "not found (correct: B has no canvas)")
else:
    line("B: marker", f"FOUND in the foreign scene at {res['B'][:4]}  <-- the locator matches anything")
    fail.append('B-control')

if res['A1'] and res['A2']:
    if res['A1'][:4] != res['A2'][:4]:
        line("A1 vs A2 box", f"{res['A1'][:4]} -> {res['A2'][:4]}  <-- the canvas MOVED"); fail.append('box')
    else:
        x0, y0, x1, y1 = res['A1'][:4]
        inner = (x0+2, y0+2, x1-2, y1-2)   # away from the antialiased edge
        for a, b in (('A1', 'A2'), ('A1', 'A3')):
            out = subprocess.run([DIFF, f"{WORK}/{a}.png", f"{WORK}/{b}.png",
                                  *map(str, inner)], capture_output=True, text=True).stdout.strip()
            tail = out.split('differing', 1)[-1].strip() if 'differing' in out else out
            line(f"{a} vs {b}: canvas px", tail)
            if not tail.startswith('0 '): fail.append(f'{a}-{b}-canvas')

# INFORMATION, NOT A GATE. The 3D behind the canvas inherits the previous scene's environment on the
# visit right after a foreign one; that is filed on its own and is not this witness's subject. Printed so
# the number is in the record and so a future reader is not surprised by it.
#
# GUARDED, because the first version of this block indexed A1 unconditionally and a RED run -- the one
# where A1 has no marker at all -- died in a traceback instead of printing its verdict. A witness whose
# failure path crashes reports a crash rather than a diagnosis.
w, h = (res['A1'][5], res['A1'][6]) if res['A1'] else decode(f"{WORK}/A1.png")[:2]
for a, b, what in (('A1', 'A2', 'after the foreign scene'), ('A1', 'A3', 'after the same scene')):
    out = subprocess.run([DIFF, f"{WORK}/{a}.png", f"{WORK}/{b}.png", '0', '0', str(w), str(h)],
                         capture_output=True, text=True).stdout.strip()
    tail = out.split('differing', 1)[-1].strip() if 'differing' in out else out
    line(f"whole frame {a}/{b}", f"{tail}   ({what})")

print()
if fail:
    print(f"WITNESS RED: {', '.join(fail)}")
    sys.exit(1)
print("WITNESS GREEN: the canvas is over the 3D in A, and the round trip through a foreign scene "
      "left it byte-identical.")
PY
status=$?
echo
echo "  the log is at $LOG (the re-install of the UI pass is the 'Added pass EditorUI2D to phase UI'"
echo "  line that must follow every 'Dropped N external pass(es)' one)."
exit $status
