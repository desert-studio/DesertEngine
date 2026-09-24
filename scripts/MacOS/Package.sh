#!/bin/bash
# THE ENGINE DROP — the downloadable build of the TOOLS, not of a game.
# Output: dist/DesertEngine-<config>/ — CI archives this directory as an artifact (ci.yml).
#
#   ./scripts/MacOS/Package.sh [Release|Debug]
#
# WHAT THIS IS AND WHAT IT IS NOT (П5). A GAME is packaged by the editor's own PackageGame() and by
# nothing else: it needs an OPEN PROJECT, which is a thing this script does not have and CI does not
# have either, and its product is the player binary plus one archive carrying the project's content
# and its descriptor. This script's product is the EDITOR plus the tools plus a project to open.
#
# WHAT TRAVELS, AND WHY IT IS NOT "Editor/Resources". Until 2026-09-11 this line was
# `cp -R Editor/Resources`, i.e. 133 MB whole, and 129 MB of that is the sandbox project's authored
# content: 50 MB of baked cloud volumes, 38 MB of meshes, 3 MB of textures and a 37 MB
# `cinematic_menu_loop.gif` that NO file in this repository names. None of it is reachable from the
# scene the drop opens. Worse, the descriptor that would have made any of it reachable —
# Editor/Desert.deproj — was not copied at all, so the packaged editor had 129 MB of assets and no
# project able to see them.
#
# Now three things travel and each has a rule rather than a list:
#   1. the ENGINE resource trees — Shaders, Fonts, Icons. Whole, because the engine's own services
#      SCAN them (Engine/Runtime/Services/ServiceScanRoots.hpp) rather than naming files; that is the
#      same census PackagedContentTrees() ships for a game, and Desert/Tests/Editor/PackagedContent
#      asserts the two agree.
#   2. the sandbox descriptor, Desert.deproj.
#   3. the CLOSURE of its DefaultScene — computed by Tools/AssetClosure, which walks the editor's own
#      asset reference graph. Not a list in this file: content moves every week and a stale list fails
#      in the direction where the package still starts and shows an empty world.
#
# WHY Content.dpak AND Content.manifest ARE NOT WRITTEN HERE, measured 2026-09-08 on the Debug drop.
# `VFS::MountPak` has exactly ONE non-test call site in this repository —
# Runtime/Source/PackagedContent.cpp — so the editor and the tools in this directory never mount an
# archive at all, and the pak's only possible reader was the Runtime sitting beside it. That reader
# mounted its 144 MB and then refused, because a pak of Editor/Resources carries no project
# descriptor and this script has no project to describe: "No game to run", reproduced by running it.
set -euo pipefail

CONFIG="${1:-Release}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/Bin/$CONFIG"
OUT="$ROOT/dist/DesertEngine-$CONFIG"
PROJECT="$ROOT/Editor/Desert.deproj"

if [ ! -x "$BIN/Runtime" ]; then
    echo "Package.sh: no $CONFIG binaries in $BIN — build first" >&2
    exit 1
fi
if [ ! -x "$BIN/AssetClosure" ]; then
    echo "Package.sh: $BIN/AssetClosure is missing." >&2
    echo "  It is what decides which assets travel. Without it this script could only guess, and a" >&2
    echo "  guess that is short by one file produces a package that starts and shows nothing." >&2
    echo "  Build it: scripts/MacOS/BuildMacOS.sh $CONFIG" >&2
    exit 1
fi
if [ ! -f "$PROJECT" ]; then
    echo "Package.sh: $PROJECT is missing — the drop has no project to open" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

for exe in Editor Runtime PakTool DShaderTool; do
    [ -x "$BIN/$exe" ] && cp "$BIN/$exe" "$OUT/"
done

# The engine resource trees, whole. `--exclude` rather than a delete pass afterwards, and the
# assertion below is what makes it checkable.
#
# MEASURED, AND THE PLACE YOU MEASURE IT DECIDES THE ANSWER: the owner's working tree holds six
# .DS_Store files under Editor/Resources (two of them inside the three trees copied here), and the
# `cp -R Editor/Resources` this replaced carried all six into the drop. A FRESH WORKTREE holds zero,
# because Finder is what creates them — so checking there says the hazard does not exist. CI checks
# out fresh too, which is why no CI artifact ever carried one and why this could stay invisible: it
# only ever affected a drop packaged on a developer's own machine, which is the one a developer
# hands to somebody.
for tree in Shaders Fonts Icons Splash; do
    if [ ! -d "$ROOT/Editor/Resources/$tree" ]; then
        echo "Package.sh: engine resource tree Editor/Resources/$tree is missing" >&2
        exit 1
    fi
    mkdir -p "$OUT/Resources/$tree"
    rsync -a --exclude='.DS_Store' --exclude='Thumbs.db' --exclude='.git*' \
        "$ROOT/Editor/Resources/$tree/" "$OUT/Resources/$tree/"
done

# The descriptor, verbatim: the drop's Resources/Assets sits exactly where the dev tree's does, so
# nothing about it needs rebasing (a GAME's does — see PackagedDescriptor()).
cp "$PROJECT" "$OUT/"

# The scene's closure. The tool's exit code is the check — its stdout is a file list and an empty one
# would be indistinguishable from "this scene needs nothing", so it refuses rather than printing none.
CLOSURE="$(mktemp)"
trap 'rm -f "$CLOSURE"' EXIT
"$BIN/AssetClosure" "$PROJECT" --out "$CLOSURE"

ASSETS_SRC="$ROOT/Editor/Resources/Assets"
ASSETS_DST="$OUT/Resources/Assets"
COPIED=0
while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ ! -f "$ASSETS_SRC/$rel" ]; then
        echo "Package.sh: AssetClosure named $rel, which is not a file under $ASSETS_SRC" >&2
        exit 1
    fi
    mkdir -p "$ASSETS_DST/$(dirname "$rel")"
    cp "$ASSETS_SRC/$rel" "$ASSETS_DST/$rel"
    COPIED=$((COPIED + 1))
done < "$CLOSURE"

if [ "$COPIED" -eq 0 ]; then
    echo "Package.sh: the closure was empty — refusing to ship a project with no content" >&2
    exit 1
fi

# NEGATIVE CONTROL, and it is here because the positive one cannot see this class at all: a package
# whose file COUNT is right can still be carrying platform junk into somebody else's operating
# system. Checked rather than trusted, because the exclusions above are three patterns in one rsync
# invocation and a fourth kind of junk would travel silently.
JUNK="$(find "$OUT" \( -name '.DS_Store' -o -name 'Thumbs.db' -o -name '__MACOSX' \) -print)"
if [ -n "$JUNK" ]; then
    echo "Package.sh: platform junk reached the package:" >&2
    echo "$JUNK" >&2
    exit 1
fi

# ── NO REGISTRY TRAVELS (AF9) ────────────────────────────────────────────────────────────────
#
# The packaged editor GATHERS its asset registry on its first start, from the headers of exactly the
# files this drop carries (Common::Content::GatherContentRegistry), and keeps it in the drop's own
# Intermediate/AssetRegistry.cache. A registry cooked here would be a second answer to the same
# question; a GAME's registry is the packager's (PackageGame writes Cooked/AssetRegistry.dreg into the pak).

if [ ! -f "$OUT/Resources/Splash/Splash.tex" ]; then
    echo "Package.sh: the drop has no Resources/Splash/Splash.tex — its splash would open without its picture" >&2
    exit 1
fi

echo "Package.sh: packaged -> $OUT"
echo "  engine resources: Shaders + Fonts + Icons + Splash (its picture is committed there)"
echo "  project assets:   $COPIED files, the closure of $(basename "$PROJECT")'s DefaultScene"
du -sh "$OUT"
