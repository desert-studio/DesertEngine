#!/usr/bin/env bash
# OpenAndShoot.sh — start a FRESH editor, open one document by name, photograph the window, close.
#
#     scripts/MacOS/OpenAndShoot.sh <project.deproj> <Open label> <out.png> [config]
#     scripts/MacOS/OpenAndShoot.sh Editor/Desert.deproj Materials/M_Clouds_Demo_Clouds.demat /tmp/m.png
#
# WHY THIS SCRIPT IS THE DELIVERABLE OF A6-1 AND NOT A CONVENIENCE.
#
# It could not be written before. An editor takes tens of seconds to come up, and the control channel
# answered `commands` throughout that window from whatever it happened to hold — 0 of this project's 130
# openable assets at first, then 106, then 130 — so every client that wanted to open something had to
# GUESS how long to wait and ask again. Two agents wrote that loop; on one tree it never terminated. A
# reply that cannot distinguish "not yet" from "there is none" makes a correct script impossible, not
# merely inconvenient.
#
# So there is no loop below, and that absence is the point:
#
#   * THE READINESS WAIT IS BY SIGNAL. One `run` is sent, once. The editor parks it until a PRESENTED
#     frame proves nothing is outstanding, then runs it and answers. The blocking read of that answer IS
#     the wait. Nothing here sleeps, and nothing here asks twice.
#   * `--wait` BELOW IS NOT THAT WAIT. It retries connect() until the SOCKET FILE exists, which is a
#     different fact — the file does not exist until the editor has called listen(), a second or two in.
#     Readiness is what happens after the connection, and it is not polled.
#   * ORDERING IS THE PROTOCOL'S. `run` returns only after a frame that already reflects it, so the
#     capture on the next line cannot photograph the state before the open. No sleep between them.
#
# Exit status: 0 the PNG is there; non-zero with the editor's own reason on stderr otherwise.

set -uo pipefail

PROJECT="${1:-}"
LABEL="${2:-}"
OUT="${3:-}"
CONFIG="${4:-Debug}"

if [ -z "$PROJECT" ] || [ -z "$LABEL" ] || [ -z "$OUT" ]; then
    echo "usage: $0 <project.deproj> <Open label> <out.png> [Debug|Release]" >&2
    echo "  the Open label is the asset's path under the project's content root, exactly as" >&2
    echo "  'desertctl commands' prints it: 'Materials/M_Clouds_Demo_Clouds.demat'." >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EDITOR_BIN="$ROOT/build/Bin/$CONFIG/Editor"
CTL="$ROOT/build/Bin/$CONFIG/DesertCtl"

for binary in "$EDITOR_BIN" "$CTL"; do
    if [ ! -x "$binary" ]; then
        echo "$0: $binary is not there. Build it: make -C build/Projects Editor config=$(echo "$CONFIG" | tr 'A-Z' 'a-z') -j8" >&2
        exit 2
    fi
done

# The project path is resolved against the CALLER's directory and the editor is then run from the
# project's own folder — a checkout's Editor/, whose parent is the engine root (Engine/Content) that
# StartupLayout's ResolveResourceRoot finds from there.
PROJECT_ABS="$(cd "$(dirname "$PROJECT")" && pwd)/$(basename "$PROJECT")"
PROJECT_DIR="$(dirname "$PROJECT_ABS")"

# NOT optional, and RunEditor.sh is where they are usually set: without these glfwVulkanSupported() is
# false and the binary dies at VulkanContext.cpp:50 before one frame, which reads as a broken build.
B="$(brew --prefix)"
export VK_ICD_FILENAMES="$B/etc/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$B/share/vulkan/explicit_layer.d"
export DYLD_FALLBACK_LIBRARY_PATH="$B/lib"

# One socket per run, named after this process: several of these run at once on this machine, and a
# shared path would connect the second run's client to the first run's editor.
SOCKET="${TMPDIR:-/tmp}/desert-openandshoot-$$.sock"
LOG="${TMPDIR:-/tmp}/desert-openandshoot-$$.log"
rm -f "$SOCKET"

( cd "$PROJECT_DIR" && "$EDITOR_BIN" --project "$(basename "$PROJECT_ABS")" --control-socket "$SOCKET" ) \
    > "$LOG" 2>&1 &
EDITOR_PID=$!

cleanup() {
    "$CTL" --socket "$SOCKET" quit 0 > /dev/null 2>&1
    # The editor segfaults during teardown (a known shutdown bug), so the wait's status says nothing and
    # is deliberately not read. The PNG was written long before this point.
    wait "$EDITOR_PID" 2>/dev/null
    rm -f "$SOCKET"
}
trap cleanup EXIT

rm -f "$OUT"

# ── ONE REQUEST, AND IT IS THE WAIT ────────────────────────────────────────────────────────────────
# The editor holds this until it has finished coming up. If it never does, the reply is a REFUSAL that
# names what stayed outstanding — not silence, and not a successful answer about a project it has not
# read. Either way this returns exactly once.
if ! "$CTL" --socket "$SOCKET" --wait 60 run Open "$LABEL"; then
    echo "$0: the editor did not open '$LABEL'. Its log is $LOG." >&2
    echo "$0: 'desertctl --socket $SOCKET commands' lists what it does offer." >&2
    exit 1
fi

# ── AND ONE CAPTURE ────────────────────────────────────────────────────────────────────────────────
# shot.window and not shot.viewport: the subject is the DOCUMENT, which is interface. The viewport
# capture reads the scene's own image and has never contained one pixel of UI.
if ! "$CTL" --socket "$SOCKET" shot-window "$OUT"; then
    echo "$0: the capture failed. Its log is $LOG." >&2
    exit 1
fi

# The channel said it wrote the file; checked anyway, because "the reply said ok" and "there is a PNG"
# are two different claims and this script's whole output is the second one.
if [ ! -s "$OUT" ]; then
    echo "$0: the editor reported a capture and $OUT is missing or empty." >&2
    exit 1
fi

echo "$0: wrote $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes) — editor log: $LOG"
