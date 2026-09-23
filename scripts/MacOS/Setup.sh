#!/usr/bin/env bash
# One-time environment setup for building Desert Engine on macOS (Apple Silicon).
# Installs every build dependency and fetches the third-party pieces that are
# not part of the repo. Safe to re-run: everything is idempotent.
#
# There is deliberately no `set -e`. The steps below are independent of one
# another, and this script used to abort on the first one that failed. A single
# unhappy submodule therefore took the optick / volk / meshoptimizer fetches
# down with it, and the only symptom anybody ever saw was premake5 refusing to
# run — which reads as "the worktree is broken", not as "setup stopped halfway".
# Every step now reports its own failure by name, the run continues, and the
# summary at the end lists everything that broke plus everything still missing.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd)"

echo "=== Desert Engine macOS setup ==="

FAILED_STEPS=()

fail() {
    FAILED_STEPS+=("$1")
    printf '    !! FAILED: %s\n' "$1" >&2
}

# ---------------------------------------------------------------------------
# 1. Homebrew packages: build tools + Vulkan (MoltenVK) + shader toolchain
# ---------------------------------------------------------------------------
if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required. Install it from https://brew.sh and re-run this script." >&2
    exit 1
fi

BREW_PACKAGES=(
    premake                 # project generator (premake5)
    cmake                   # used by some third-party builds
    vulkan-headers          # <vulkan/vulkan.h>
    vulkan-loader           # libvulkan.dylib
    molten-vk               # Vulkan-over-Metal driver (ICD)
    vulkan-validationlayers # VK_LAYER_KHRONOS_validation (Debug builds)
    shaderc                 # libshaderc_combined.a — runtime GLSL compilation
    spirv-cross             # SPIR-V reflection/translation
    assimp                  # model importing (Editor, FbxMeshSplitter)
    googletest              # unit tests (optional, --with-tests builds)
    # A BUILD DEPENDENCY, NOT A CONVENIENCE. The test suites compile engine translation units into
    # themselves rather than linking libDesert, so 505 of the 1940 compiles in a Debug build are a
    # repeat of a (source, flags) pair already compiled in the SAME build — 30.8 % of the compile CPU,
    # measured. scripts/MacOS/BuildMacOS.sh turns those into cache hits when this is present and says
    # so loudly when it is not; see the block there for what makes a wrong hit impossible.
    ccache                  # compiler cache — see scripts/MacOS/BuildMacOS.sh
)

# WHY llvm@18 IS A DEVELOPER DEPENDENCY AND NOT A BUILD ONE, and why it is nevertheless installed
# here. Every push is gated on scripts/CI/CheckFormat.sh, which CI runs against clang-format 18 and
# nothing else (ci.yml pins the `clang-format-18` package deliberately, with the reasoning written out
# beside it). A developer checking with whatever `clang-format` their machine happens to have gets a
# DIFFERENT verdict — v22 and v18 disagree on parameter-less multi-line lambdas, on multi-variable
# declarations split across lines, and on aligned const-declaration blocks — and the symptom is a red
# CI run on code that was clean locally. That happened three times in one day.
#
# NOT ON CI, and the exclusion is measured rather than tidy: llvm@18 is a ~1.5 GB bottle and the
# `sanitizers` job already sits at 61-73 % of its 90-minute ceiling. Paying that on every macOS run is
# how a job crosses its timeout, and a timed-out job reports as `cancelled`, which looks like nothing
# being wrong.
#
# EXACTLY ONE CI JOB NEEDS THE KEG AND IT INSTALLS THE KEG ITSELF: `tidy` in ci.yml runs
# scripts/CI/CheckTidy.sh, whose clang-tidy is pinned to the same llvm@18 the formatter uses. It has
# its own `brew install llvm@18` step for that reason, so the cost lands on the one job that spends it
# rather than on the two builds that do not. (The format gate is a separate ubuntu job with an apt
# package and never touches this script at all.)
#
# Keg-only, so it is deliberately NOT linked into PATH: `clang-format` there would shadow the system
# one for everything else on the machine. The check in step 5 prints the one PATH prefix that runs it.
if [ -z "${CI:-}" ]; then
    BREW_PACKAGES+=(llvm@18)
fi

echo "--- Installing Homebrew packages: ${BREW_PACKAGES[*]}"
for pkg in "${BREW_PACKAGES[@]}"; do
    if brew list --formula "$pkg" >/dev/null 2>&1; then
        echo "    $pkg: already installed"
    elif brew install "$pkg"; then
        echo "    $pkg: installed"
    else
        fail "brew install $pkg"
    fi
done

# ---------------------------------------------------------------------------
# 2. Git submodules
#
# `git submodule update --init --recursive` walks every submodule in one
# process and dies on the first one it cannot resolve, leaving each later
# submodule uninitialised. Drive the walk one top-level submodule at a time so
# a failure is confined to the submodule that caused it.
# ---------------------------------------------------------------------------
echo "--- Initializing git submodules"

# Where git keeps a submodule's real repository. In a linked worktree that is
# under .git/worktrees/<name>/modules, not .git/modules — print both candidates.
submodule_gitdirs() {
    local sm_path="$1"
    local d
    if d="$(git rev-parse --absolute-git-dir 2>/dev/null)" && [ -n "$d" ]; then
        echo "$d/modules/$sm_path"
    fi
    if d="$(git rev-parse --git-common-dir 2>/dev/null)" && [ -n "$d" ]; then
        case "$d" in
            /*) : ;;
            *) d="$ROOT/$d" ;;
        esac
        echo "$d/modules/$sm_path"
    fi
}

# Explain the one failure mode that keeps getting mis-diagnosed as a bad
# gitlink: a submodule directory that already has content is never re-cloned,
# so if its .git pointer names a gitdir that does not exist, git dies with
# "Unable to find current revision" / "could not get a repository handle" and
# names the submodule as though its recorded commit were at fault.
diagnose_submodule() {
    local sm_path="$1"
    [ -f "$sm_path/.git" ] || return 0
    local target
    target="$(sed -n 's/^gitdir: //p' "$sm_path/.git" 2>/dev/null)"
    [ -n "$target" ] || return 0
    case "$target" in
        /*) : ;;
        *) target="$sm_path/$target" ;;
    esac
    if [ ! -d "$target" ]; then
        echo "       its .git pointer names $target, which does not exist."
        echo "       That is what copying ThirdParty/ between two checkouts leaves behind:"
        echo "       the pointer is a relative path, and it only resolves in its original tree."
    fi
}

# A submodule whose clone succeeded but whose checkout did not leaves a
# directory holding nothing but the .git pointer. git treats that as up to
# date: `git submodule status` prints it clean at the right commit, and
# `git submodule update` exits 0 without touching it, for ever. Checking the
# exit status alone therefore cannot tell a populated submodule from an empty
# one — look at the working tree.
submodule_is_populated() {
    local sm_path="$1"
    [ -n "$(ls -A "$sm_path" 2>/dev/null | grep -v '^\.git$')" ]
}

# Local state only. Never touches .gitmodules or the recorded commit.
reset_submodule_state() {
    local sm_path="$1"
    # Only meaningful when the working tree actually has content: in an empty
    # submodule `git status` reports every tracked file as deleted, which is
    # not a local modification worth warning about.
    if submodule_is_populated "$sm_path" &&
        git -C "$sm_path" rev-parse --git-dir >/dev/null 2>&1 &&
        [ -n "$(git -C "$sm_path" status --porcelain 2>/dev/null)" ]; then
        echo "       WARNING: discarding local modifications in $sm_path" >&2
    fi
    git submodule deinit -f -- "$sm_path" >/dev/null 2>&1
    local d
    while IFS= read -r d; do
        [ -n "$d" ] && rm -rf "$d"
    done < <(submodule_gitdirs "$sm_path")
    rm -rf "${ROOT:?}/$sm_path"
}

SUBMODULE_PATHS=()
while IFS= read -r sm_path; do
    [ -n "$sm_path" ] && SUBMODULE_PATHS+=("$sm_path")
done < <(git ls-files --stage 2>/dev/null | awk '$1 == "160000" { print $4 }')

if [ ${#SUBMODULE_PATHS[@]} -eq 0 ]; then
    fail "no submodules found in the git index (is this a git checkout?)"
else
    for sm_path in "${SUBMODULE_PATHS[@]}"; do
        if git submodule update --init --recursive -- "$sm_path" &&
            submodule_is_populated "$sm_path"; then
            echo "    $sm_path: ok"
            continue
        fi

        if submodule_is_populated "$sm_path"; then
            echo "    $sm_path: failed; resetting its local state and retrying once"
            diagnose_submodule "$sm_path"
        else
            echo "    $sm_path: git calls it up to date but its working tree is empty;" \
                "resetting its local state and retrying once"
        fi
        reset_submodule_state "$sm_path"

        if git submodule update --init --recursive -- "$sm_path" &&
            submodule_is_populated "$sm_path"; then
            echo "    $sm_path: ok (after reset)"
        elif submodule_is_populated "$sm_path"; then
            fail "git submodule update --init --recursive -- $sm_path"
        else
            fail "$sm_path is still empty after a reset and a re-clone"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 3. Third-party sources that are not submodules.
# ---------------------------------------------------------------------------
# clone_dep <sentinel-file> <dest-dir> <url> [branch]
clone_dep() {
    local sentinel="$1" dir="$2" url="$3" branch="${4:-}"
    local name="${dir#"$ROOT"/}"

    if [ -f "$sentinel" ]; then
        echo "--- $name: present"
        return 0
    fi

    echo "--- Cloning $name"
    rm -rf "$dir"
    if [ -n "$branch" ]; then
        git clone --branch "$branch" --depth 1 "$url" "$dir" || {
            fail "git clone $url -> $name"
            return 1
        }
    else
        git clone --depth 1 "$url" "$dir" || {
            fail "git clone $url -> $name"
            return 1
        }
    fi

    if [ ! -f "$sentinel" ]; then
        fail "$name was cloned but ${sentinel#"$ROOT"/} is missing"
        return 1
    fi
    return 0
}

# Optick profiler sources.
clone_dep "$ROOT/ThirdParty/optick/src/optick.h" \
    "$ROOT/ThirdParty/optick" \
    https://github.com/bombomby/optick.git

# volk (Vulkan meta-loader) headers — shipped by the LunarG SDK on Windows,
# vendored here since Homebrew has no formula for it.
clone_dep "$ROOT/ThirdParty/volk/volk.h" \
    "$ROOT/ThirdParty/volk" \
    https://github.com/zeux/volk.git

# meshoptimizer (mesh simplification / LOD generation).
clone_dep "$ROOT/ThirdParty/meshoptimizer/src/meshoptimizer.h" \
    "$ROOT/ThirdParty/meshoptimizer" \
    https://github.com/zeux/meshoptimizer.git \
    v0.20

# ---------------------------------------------------------------------------
# 4. reflect-cpp compiled sources. The vendored include/ headers are reflect-cpp
#    v0.19.0 and need rfl::Generic / rfl::json / yyjson built from the matching
#    sources (all platforms compile them via BuildScripts/ThirdParty/ReflectCpp.lua).
#    These sources are committed to the repo, so this normally does nothing.
# ---------------------------------------------------------------------------
RCPP_SRC="$ROOT/ThirdParty/reflect-cpp/src"
if [ -f "$RCPP_SRC/reflectcpp.cpp" ]; then
    echo "--- ThirdParty/reflect-cpp: sources present"
else
    echo "--- Fetching reflect-cpp v0.19.0 sources"
    TMP_RCPP="$(mktemp -d)"
    if git clone --branch v0.19.0 --depth 1 https://github.com/getml/reflect-cpp "$TMP_RCPP"; then
        mkdir -p "$RCPP_SRC"
        if cp "$TMP_RCPP/src/reflectcpp.cpp" "$TMP_RCPP/src/reflectcpp_json.cpp" \
            "$TMP_RCPP/src/yyjson.c" "$RCPP_SRC/" &&
            cp -R "$TMP_RCPP/src/rfl" "$RCPP_SRC/"; then
            echo "    ThirdParty/reflect-cpp: fetched"
        else
            fail "copying reflect-cpp sources into ThirdParty/reflect-cpp/src"
        fi
    else
        fail "git clone reflect-cpp v0.19.0"
    fi
    rm -rf "$TMP_RCPP"
fi

# ---------------------------------------------------------------------------
# 5. Final check: is everything premake5 and the generated makefiles read
#    actually on disk? A setup that finishes silently incomplete is the reason
#    this was mis-diagnosed as a broken worktree over and over.
# ---------------------------------------------------------------------------
echo "--- Verifying the paths the build reads"

# premake5 raises a hard error of its own for the first three. Everything after
# them is pulled in by a wildcard, so when it is missing premake5 still succeeds
# and emits a project with no source files — the failure only surfaces later as
# "No rule to make target ...". Name them here instead.
#
# Three submodules are deliberately absent from this list because no premake
# file and no source in the tree refers to them: ThirdParty/NVRHI,
# ThirdParty/lightweightvk and Editor/ThirdParty/ImGuiColorTextEdit. They are
# still initialised above, and a failure to initialise one is still reported —
# but the build does not need them, so their absence must not be reported as a
# missing build input.
REQUIRED_PATHS=(
    "ThirdParty/optick/src/optick.h"
    "ThirdParty/meshoptimizer/src/meshoptimizer.h"
    "ThirdParty/reflect-cpp/src/reflectcpp.cpp"
    "ThirdParty/volk/volk.h"
    "ThirdParty/GLFW/include/GLFW/glfw3.h"
    "ThirdParty/ImGui/imgui.cpp"
    "ThirdParty/imgui-node-editor/imgui_node_editor.cpp"
    "ThirdParty/yaml-cpp/src/parser.cpp"
    "ThirdParty/JoltPhysics/Jolt/Jolt.h"
    "ThirdParty/lua/lapi.c"
    "ThirdParty/spdlog/include/spdlog/spdlog.h"
    "ThirdParty/sol2/include/sol/sol.hpp"
    "Editor/ThirdParty/ImGuizmo/ImGuizmo.cpp"
    "ThirdParty/reflect-cpp/include"
    "ThirdParty/google-test/include/gtest/gtest.h"
    "ThirdParty/glm/glm/glm.hpp"
    "ThirdParty/entt/include/entt/entt.hpp"
    "ThirdParty/stb/include"
    "ThirdParty/VulkanAllocator"
    "Editor/ThirdParty/assimp/include"
)

MISSING_PATHS=()
for required in "${REQUIRED_PATHS[@]}"; do
    [ -e "$ROOT/$required" ] || MISSING_PATHS+=("$required")
done

# The format gate's binaries. Checked by RUNNING one, because "llvm@18 is installed" and "the gate
# can run" are two different facts: the formula is keg-only, so its bin/ is not on PATH, and
# CheckFormat.sh looks for clang-format-18 or clang-format and by default finds neither.
if [ -z "${CI:-}" ]; then
    FORMAT_PREFIX="$(brew --prefix llvm@18 2>/dev/null || true)"
    if [ -n "$FORMAT_PREFIX" ] && [ -x "$FORMAT_PREFIX/bin/clang-format" ] &&
        [ -x "$FORMAT_PREFIX/bin/git-clang-format" ]; then
        echo "    format gate: $("$FORMAT_PREFIX/bin/clang-format" --version)"
        echo "    run it with:  PATH=\"$FORMAT_PREFIX/bin:\$PATH\" ./scripts/CI/CheckFormat.sh <merge-base>"
    else
        fail "llvm@18's clang-format / git-clang-format are not runnable (scripts/CI/CheckFormat.sh needs them)"
    fi
fi

if [ ${#MISSING_PATHS[@]} -eq 0 ]; then
    echo "    all ${#REQUIRED_PATHS[@]} required paths present"
else
    for missing in "${MISSING_PATHS[@]}"; do
        echo "    MISSING: $missing"
    done
fi

# ---------------------------------------------------------------------------
# 6. Summary
# ---------------------------------------------------------------------------
echo ""
if [ ${#FAILED_STEPS[@]} -eq 0 ] && [ ${#MISSING_PATHS[@]} -eq 0 ]; then
    echo "=== Setup complete ==="
    echo "Build with:  scripts/MacOS/BuildMacOS.sh [Debug|Release|Shipping]"
    exit 0
fi

echo "=== Setup INCOMPLETE ==="

if [ ${#FAILED_STEPS[@]} -gt 0 ]; then
    echo ""
    echo "Steps that failed (${#FAILED_STEPS[@]}):"
    for step in "${FAILED_STEPS[@]}"; do
        echo "  - $step"
    done
fi

if [ ${#MISSING_PATHS[@]} -gt 0 ]; then
    echo ""
    echo "Paths the build needs that are still missing (${#MISSING_PATHS[@]}):"
    for missing in "${MISSING_PATHS[@]}"; do
        echo "  - $missing"
    done
fi

echo ""
if [ ${#MISSING_PATHS[@]} -eq 0 ]; then
    echo "Every path the build reads is nevertheless present, so premake5 gmake"
    echo "should succeed. Fix the failed steps above before relying on this tree."
else
    echo "premake5 will not produce a working build until the missing paths above"
    echo "are resolved."
fi
echo "Re-running this script is safe: it retries every step that failed."
echo ""
echo "If one submodule keeps failing, reset that one by hand and retry it:"
echo "    git submodule deinit -f -- <path>"
echo "    rm -rf \"\$(git rev-parse --absolute-git-dir)/modules/<path>\" <path>"
echo "    git submodule update --init --recursive -- <path>"
echo ""
echo "Do not copy ThirdParty/ from another checkout to work around this. Each"
echo "submodule's .git file holds a relative path to its gitdir, which resolves"
echo "only in the tree it was created in; copying it is what produces the"
echo "\"Unable to find current revision\" failure this script now repairs."
exit 1
