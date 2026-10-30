#!/usr/bin/env bash
# Launch the Desert Editor built by BuildMacOS.sh.
#
# Usage: scripts/MacOS/RunEditor.sh [Debug|Release] [editor args...]
#        (everything after the config is forwarded to the Editor, e.g. --project <path>)
set -euo pipefail

cd "$(dirname "$0")/../.."

CONFIG="${1:-Debug}"
if [ $# -gt 0 ]; then shift; fi
EDITOR="build/Bin/$CONFIG/Editor"

if [ ! -x "$EDITOR" ]; then
    echo "$EDITOR not found — build first: scripts/MacOS/BuildMacOS.sh $CONFIG" >&2
    exit 1
fi

# STALE-BUILD GUARD: a binary older than the sources looks like a broken engine, not a stale
# build (a day-old Release once choked on 43 shaders whose DSL its includer didn't know yet).
NEWEST_SRC=$(find Desert/Desert/Source Desert/Common/Source Editor/Source -name '*.cpp' -newer "$EDITOR" -print -quit 2>/dev/null)
if [ -n "$NEWEST_SRC" ]; then
    echo "WARNING: $EDITOR is OLDER than $NEWEST_SRC" >&2
    echo "         rebuild first: scripts/MacOS/BuildMacOS.sh $CONFIG" >&2
fi

# Validation layers come from Homebrew; the MoltenVK ICD does not (below).
BREW_PREFIX="${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}"
export VK_LAYER_PATH="${VK_LAYER_PATH:-$BREW_PREFIX/share/vulkan/explicit_layer.d}"

# THE DRIVER IS THE REPO'S: the pinned MoltenVK built by scripts/MacOS/BuildMoltenVK.sh, not Homebrew's
# molten-vk (1.4.2 warns about blending on every non-blendable colour slot, blendEnable unread). A
# missing build is a refusal, not a fall-back to whatever driver the loader would find.
VK_ICD_JSON="$(bash scripts/MacOS/BuildMoltenVK.sh --icd)" || exit 1
export VK_ICD_FILENAMES="$VK_ICD_JSON"
export VK_DRIVER_FILES="$VK_ICD_JSON"

# GLFW loads the Vulkan loader with dlopen("libvulkan.1.dylib"), and dyld does
# not search the Homebrew prefix by default.

# Where this engine lives, so the Editor can record itself in ~/.desertengine/engines.json — the
# file the launcher reads to find an engine at all. Exported BEFORE the `cd`, because after it the
# working directory is Editor/ and the root would have to be re-derived from it.
export DESERT_ROOT="$PWD"

# The engine resolves Resources/... relative to the working directory.
cd Editor

# The editor REQUIRES a project (--project <.deproj>); picking projects is the Project Hub's job.
# With no extra args, open the sample project (Projects/Desert); there is no content without a project.
if [ $# -eq 0 ]; then
    set -- --project ../Projects/Desert/Desert.deproj
fi

exec "../$EDITOR" "$@"
