#!/usr/bin/env bash
# MoltenVK, BUILT FROM SOURCE AT A PINNED COMMIT (the assimp pattern: the version is the repo's, not
# whatever Homebrew shipped this week).
#
# Why not Homebrew's molten-vk: 1.4.2 (the last release, 2026-07-24) warns "Blending is enabled ...
# but the format does not support blending" for EVERY colour attachment whose format cannot blend
# (our R32_UINT GBuffer slots), without reading blendEnable (MVKPipeline.mm:1990). Upstream fixed it
# in f92b64c (issue #2811, 2026-08-25) with no release since. Our pipelines leave BlendEnable=false
# on those slots, so the warning is MoltenVK's defect, and the fix is the driver, not a log filter.
#
# PINNED: KhronosGroup/MoltenVK main @ MVK_COMMIT (2026-10-13, contains f92b64c). Bump it here only.
#
# Output (shared by every worktree on the machine, one build per commit):
#   $DESERT_THIRDPARTY/MoltenVK/<commit>/icd/libMoltenVK.dylib
#   $DESERT_THIRDPARTY/MoltenVK/<commit>/icd/MoltenVK_icd.json   <- VK_ICD_FILENAMES points here
#
# Usage: scripts/MacOS/BuildMoltenVK.sh          build (idempotent: an existing build is kept)
#        scripts/MacOS/BuildMoltenVK.sh --icd    print the ICD json path; exit 1 when not built
#                                                (RunEditor.sh/RunRuntime.sh export it as VK_ICD_FILENAMES)
set -euo pipefail

MVK_COMMIT="5189f8de86ec365860f63bf1da736815cfaea5cd"
MVK_FIX_COMMIT="f92b64c3a5084c6cfa78f581e0b863b40d2d16fa"
MVK_URL="https://github.com/KhronosGroup/MoltenVK.git"

ROOT="${DESERT_THIRDPARTY:-$HOME/.desert-thirdparty}/MoltenVK/$MVK_COMMIT"
SRC="$ROOT/src"
ICD="$ROOT/icd"

if [ "${1:-}" = "--icd" ]; then
    if [ -f "$ICD/libMoltenVK.dylib" ] && [ -f "$ICD/MoltenVK_icd.json" ]; then
        echo "$ICD/MoltenVK_icd.json"
        exit 0
    fi
    echo "[MoltenVK] pinned MoltenVK $MVK_COMMIT is not built: run scripts/MacOS/BuildMoltenVK.sh" >&2
    exit 1
fi

if [ -f "$ICD/libMoltenVK.dylib" ] && [ -f "$ICD/MoltenVK_icd.json" ]; then
    echo "[MoltenVK] already built: $ICD"
    exit 0
fi

command -v xcodebuild >/dev/null || { echo "[MoltenVK] xcodebuild not found (full Xcode >= 15.0.1 required)" >&2; exit 1; }

mkdir -p "$ROOT"
if [ ! -d "$SRC/.git" ]; then
    git clone --filter=blob:none "$MVK_URL" "$SRC"
fi
git -C "$SRC" fetch --quiet origin
git -C "$SRC" checkout --quiet --detach "$MVK_COMMIT"
git -C "$SRC" merge-base --is-ancestor "$MVK_FIX_COMMIT" HEAD \
    || { echo "[MoltenVK] pinned commit lacks the blend-warning fix $MVK_FIX_COMMIT" >&2; exit 1; }

cd "$SRC"
./fetchDependencies --macos
make macos

DYLIB="$SRC/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"
[ -f "$DYLIB" ] || { echo "[MoltenVK] build finished but $DYLIB is missing" >&2; exit 1; }

mkdir -p "$ICD"
cp "$DYLIB" "$ICD/libMoltenVK.dylib"
cat > "$ICD/MoltenVK_icd.json" <<EOF
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "./libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
EOF
echo "[MoltenVK] built $MVK_COMMIT -> $ICD"
