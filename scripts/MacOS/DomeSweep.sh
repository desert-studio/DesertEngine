#!/usr/bin/env bash
#
# DomeSweep — shoot the WHOLE celestial dome for one scene and assemble the tiles into one labelled
# contact sheet.
#
# WHY. The protocol this replaces was six look directions, and six rays is a sample. An empty zenith
# above ~20 degrees lived through a ten-merge programme because every frame was shot from the horizon,
# and the full-width bands of REVIEW_622a01a6.md needed sunward azimuth AND high elevation at the same
# time, so no protocol that varies one axis at a time could ever have produced the frame that showed
# them. This sweeps both axes together.
#
# THE SCRIPT DOES NOT COMPUTE ANY ANGLES. It asks DomeSheet for the plan and passes the vectors through
# untouched, so the label burnt into a tile and the ray the editor was pointed along cannot drift apart;
# that agreement is what Desert/Tests/Tools/DomeSheet asserts. A driver that formatted its own labels
# would be a second, untested implementation of the same arithmetic.
#
# EVERY TILE IS CHECKED AGAINST ITS OWN LOG. `--shot` refuses a scene it cannot load now, but the
# programme has already lost one row of measurements to a capture that photographed the wrong subject
# and exited 0, so the confirmation line is grepped for rather than assumed.
#
# Usage:
#   scripts/MacOS/DomeSweep.sh <scene.desce> <outdir> [options]
#
#   --camera x,y,z        (default 0,200,0)
#   --frames N            (default 90)
#   --elevations a,b,c    (default 5,25,45,65,85)
#   --azimuths N          (default 8)
#   --scale K             sheet reduction, integer (default 4)
#   --prefix NAME         tile file prefix (default: the scene's stem)
#   --config Debug|Release (default Debug)
#   --no-play             leave the world frozen; the default advances gameplay time
#   --keep-tiles          do not delete the full-size tiles after the sheet is built
#
# The scene path is relative to Editor/, exactly as `--scene` wants it.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

SCENE=""
OUTDIR=""
CAMERA="0,200,0"
FRAMES=90
ELEVATIONS="5,25,45,65,85"
AZIMUTHS=8
SCALE=4
PREFIX=""
CONFIG="Debug"
PLAY="--play"
KEEP_TILES=0

while [ $# -gt 0 ]; do
    case "$1" in
        --camera)     CAMERA="$2"; shift 2 ;;
        --frames)     FRAMES="$2"; shift 2 ;;
        --elevations) ELEVATIONS="$2"; shift 2 ;;
        --azimuths)   AZIMUTHS="$2"; shift 2 ;;
        --scale)      SCALE="$2"; shift 2 ;;
        --prefix)     PREFIX="$2"; shift 2 ;;
        --config)     CONFIG="$2"; shift 2 ;;
        --no-play)    PLAY=""; shift ;;
        --keep-tiles) KEEP_TILES=1; shift ;;
        -*)
            echo "DomeSweep: unrecognised option '$1'" >&2
            exit 2 ;;
        *)
            if [ -z "$SCENE" ]; then SCENE="$1"
            elif [ -z "$OUTDIR" ]; then OUTDIR="$1"
            else echo "DomeSweep: unexpected argument '$1'" >&2; exit 2
            fi
            shift ;;
    esac
done

if [ -z "$SCENE" ] || [ -z "$OUTDIR" ]; then
    echo "usage: DomeSweep.sh <scene.desce, relative to Editor/> <outdir> [options]" >&2
    exit 2
fi

EDITOR="$ROOT/build/Bin/$CONFIG/Editor"
DOMESHEET="$ROOT/build/Bin/$CONFIG/DomeSheet"
for binary in "$EDITOR" "$DOMESHEET"; do
    if [ ! -x "$binary" ]; then
        echo "DomeSweep: '$binary' is missing. Build it: make -C build/Projects Editor config=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]') -j8" >&2
        exit 1
    fi
done
if [ ! -f "$ROOT/Editor/$SCENE" ]; then
    echo "DomeSweep: scene '$ROOT/Editor/$SCENE' does not exist" >&2
    exit 1
fi

if [ -z "$PREFIX" ]; then
    PREFIX="$(basename "$SCENE" .desce)"
fi

mkdir -p "$OUTDIR"
TILEDIR="$OUTDIR/tiles"
mkdir -p "$TILEDIR"

# The three environment variables RunEditor.sh exports. Without them glfwVulkanSupported() is false and
# the binary dies before the first frame, which reads as a broken build rather than a bare shell.
BREW_PREFIX="$(brew --prefix)"
export VK_ICD_FILENAMES="$BREW_PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$BREW_PREFIX/share/vulkan/explicit_layer.d"
export DYLD_FALLBACK_LIBRARY_PATH="$BREW_PREFIX/lib"

PLAN="$OUTDIR/plan.tsv"
"$DOMESHEET" --plan --elevations "$ELEVATIONS" --azimuths "$AZIMUTHS" > "$PLAN"
TILE_COUNT=$(wc -l < "$PLAN" | tr -d ' ')
echo "DomeSweep: $TILE_COUNT tiles, $FRAMES frames each, scene $SCENE"

SHEET_ARGS=()
INDEX=0
FAILED=0

while IFS=$'\t' read -r STEM LOOK LABEL; do
    INDEX=$((INDEX + 1))
    TILE="$TILEDIR/${PREFIX}_${STEM}.png"
    LOG="$TILE.log"

    printf '[%2d/%2d] %-14s look %-28s ' "$INDEX" "$TILE_COUNT" "$LABEL" "$LOOK"

    # The editor segfaults during teardown after writing the PNG — a known shutdown bug — so its exit
    # status says nothing about whether the capture succeeded. The LOG does, and it is the only thing
    # this script believes.
    ( cd "$ROOT/Editor" && "$EDITOR" --project Desert.deproj --scene "$SCENE" \
        --shot "$TILE" --shot-frames "$FRAMES" $PLAY \
        --camera "$CAMERA" --look "$LOOK" ) > "$LOG" 2>&1 || true

    if ! grep -q "\[Shot\] wrote -> $TILE" "$LOG"; then
        echo "NO CONFIRMATION IN LOG"
        FAILED=$((FAILED + 1))
        continue
    fi
    if [ ! -s "$TILE" ]; then
        echo "PNG MISSING"
        FAILED=$((FAILED + 1))
        continue
    fi
    echo "ok"

    SHEET_ARGS+=( "$TILE" "$LABEL" )
done < "$PLAN"

if [ "$FAILED" -ne 0 ]; then
    echo "DomeSweep: $FAILED of $TILE_COUNT tiles did not render; the sheet would be a lie. Stopping." >&2
    exit 1
fi

SHEET="$OUTDIR/${PREFIX}_dome.png"
TITLE="$PREFIX  CAM $CAMERA  ${FRAMES}F$( [ -n "$PLAY" ] && echo ' PLAY' )  AZ0=-Z  AZ180=+Z"

"$DOMESHEET" --out "$SHEET" --title "$TITLE" --cols "$AZIMUTHS" --scale "$SCALE" \
    "${SHEET_ARGS[@]}"

if [ "$KEEP_TILES" -eq 0 ]; then
    # The repository is 5.6 GB and Docs/Clouds/Shots alone is 508 MB. A full dome is forty 1.2 MB
    # frames; the SHEET is the deliverable and the tiles are intermediate, so they go unless asked for.
    rm -rf "$TILEDIR"
    echo "DomeSweep: tiles deleted (--keep-tiles to keep them)"
fi

echo "DomeSweep: $SHEET"
