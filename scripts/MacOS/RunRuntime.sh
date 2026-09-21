#!/usr/bin/env bash
# Launch the Desert Runtime (standalone player) for a project.
#
# Usage: scripts/MacOS/RunRuntime.sh [Debug|Release|Shipping] [--project <.deproj>] [--scene <.desce>]
#
# A Shipping Runtime has no --shot and no --shot-frames in it: the capture is compiled out (see
# Common/Core/DevInstruments.hpp). Passing them is not an error there, it is a pair of tokens nothing
# reads — photograph that build from outside the process (screencapture -l) instead.
#        (with no --project, falls back to the built-in sandbox project)
set -euo pipefail

cd "$(dirname "$0")/../.."

CONFIG="${1:-Debug}"
if [ $# -gt 0 ]; then shift; fi
RUNTIME="build/Bin/$CONFIG/Runtime"

if [ ! -x "$RUNTIME" ]; then
    echo "$RUNTIME not found — build first: scripts/MacOS/BuildMacOS.sh $CONFIG" >&2
    exit 1
fi

BREW_PREFIX="${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$BREW_PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json}"
export VK_LAYER_PATH="${VK_LAYER_PATH:-$BREW_PREFIX/share/vulkan/explicit_layer.d}"
export DYLD_FALLBACK_LIBRARY_PATH="$BREW_PREFIX/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"

# Engine resources (shaders/fonts) resolve relative to the working directory — same tree the editor uses.
cd Editor

if [ $# -eq 0 ]; then
    set -- --project Desert.deproj
fi

exec "../$RUNTIME" "$@"
