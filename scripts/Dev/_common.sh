#!/usr/bin/env bash
# Shared by scripts/Dev/*.sh: the tree root, a per-run log directory, and a per-test time cap.
# Logs live under build/DevLogs (ignored by git, one copy per worktree) so parallel agents never share them.

DEV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The host's configure preset (CMakePresets.json) and its build tree. Ninja Multi-Config: one tree holds Debug, Release
# and Shipping, each with its own build-<Config>.ninja; the dev scripts build and run DEV_CONFIG, the host's row of
# scripts/Dev/dev_config.txt (play_scenario.py reads the same row). A host without a row is an error, not a default.
case "$(uname -s)" in
    Darwin) DEV_PRESET="macos-clang-ninja"; DEV_HOST=Darwin ;;
    *)      DEV_PRESET="windows-msvc-ninja"; DEV_HOST=Windows ;;
esac
DEV_BUILD="$DEV_ROOT/build/cmake/$DEV_PRESET"
DEV_CONFIG="$(awk -v host="$DEV_HOST" '$1 == host { print $2 }' "$DEV_ROOT/scripts/Dev/dev_config.txt" 2>/dev/null)"
[ -n "$DEV_CONFIG" ] || { echo "_common.sh: scripts/Dev/dev_config.txt has no row for $DEV_HOST" >&2; return 1 2>/dev/null || exit 1; }
DEV_BIN="$DEV_ROOT/build/Bin/$DEV_CONFIG"

# dev_logdir <name> — creates and prints build/DevLogs/<name>-<timestamp>
dev_logdir() {
    local d="$DEV_ROOT/build/DevLogs/$1-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$d" && echo "$d"
}

# dev_capped <seconds> <cmd...> — runs cmd; after <seconds> SIGKILLs it and returns 124 (macOS has no `timeout`).
# A watchdog parent, not `alarm; exec`: a test that blocks or handles SIGALRM outlived that cap by 40 minutes.
#
# The Homebrew validation layer's manifest names its library bare (libVkLayer_khronos_validation.dylib), and dyld's
# default fallback on Apple Silicon is /usr/lib only, so every Vulkan suite got VK_ERROR_LAYER_NOT_PRESENT at
# vkCreateInstance (RDG-MAC1). DYLD_* cannot be exported from here: SIP strips it on the way through /usr/bin/env
# and /usr/bin/perl, so perl sets it in the child itself, before exec.
DEV_LAYER_DIR=""
[ "$(uname)" = Darwin ] && [ -f /opt/homebrew/lib/libVkLayer_khronos_validation.dylib ] && DEV_LAYER_DIR=/opt/homebrew/lib
export DEV_LAYER_DIR
dev_capped() {
    perl -e '$t = shift; $p = fork;
             if (!$p) { if ($ENV{DEV_LAYER_DIR}) { $ENV{DYLD_FALLBACK_LIBRARY_PATH} = $ENV{DEV_LAYER_DIR} } exec @ARGV; exit 127 }
             $SIG{ALRM} = sub { kill 9, $p; waitpid $p, 0; exit 124 }; alarm $t; waitpid $p, 0;
             exit( ($? & 127) ? 128 + ($? & 127) : $? >> 8 )' "$@"
}

# dev_help <file> — prints the script's leading comment block (the 5-line --help).
dev_help() {
    sed -n '2,/^[^#]/p' "$1" | sed -n '/^#/s/^# \{0,1\}//p'
}

# dev_configure <log dir> [A,B,...] — configure the host preset once; again only when the DevTests suite list changes.
# A merge that adds or removes sources needs nothing here: every source glob is CONFIGURE_DEPENDS, so the next build
# re-globs and re-configures by itself. The suite list (suite.sh --dev-suites) is the one input a build cannot see,
# so it is compared with the cache: a plain call after a --dev-suites one configures DevTests out again.
dev_configure() {
    local logdir="$1" suites="${2:-}" current
    current="$(sed -n 's/^DESERT_DEV_SUITES:STRING=//p' "$DEV_BUILD/CMakeCache.txt" 2>/dev/null)"
    [ -f "$DEV_BUILD/build.ninja" ] && [ "$current" = "$suites" ] && return 0
    (cd "$DEV_ROOT" && cmake --preset "$DEV_PRESET" -DDESERT_DEV_SUITES="$suites") >"$logdir/configure.log" 2>&1 ||
        { echo "cmake --preset $DEV_PRESET FAILED; log $logdir/configure.log"; tail -3 "$logdir/configure.log"; return 1; }
}

# dev_has_target <name> — the DEV_CONFIG build defines that target (a runner, a tool, Editor).
dev_has_target() {
    grep -q "^build $1: phony" "$DEV_BUILD/build-$DEV_CONFIG.ninja" 2>/dev/null
}

# dev_up_to_date <name> — Ninja, asked in dry run, has nothing left to do for the target: it is newer than every object,
# library and generated file it depends on. The build's own dependency graph answers, not a timestamp guess.
dev_up_to_date() {
    # CONFIGURE_DEPENDS globs (desert_glob) make the glob check an always-dirty phony edge, so a dry run lists it and
    # the CMake re-run behind it after every build ("[0/2] Re-checking globbed directories...", "[1/2] Re-running
    # CMake..."). Any OTHER step is real work left for the target.
    local out
    out=$(ninja -C "$DEV_BUILD" -f "build-$DEV_CONFIG.ninja" -n "$1" 2>/dev/null) || return 1
    grep -q '^ninja: no work to do' <<<"$out" && return 0
    ! grep -E '^\[[0-9]+/[0-9]+\] ' <<<"$out" | grep -vqE '\] ((Re-checking globbed directories|Re-running CMake)\.\.\.|GenVersion: .*|DesertHeaderTool: .*)$'
}

# dev_drop_foreign_pch — deletes every DEV_CONFIG precompiled header of this tree that another tree compiled, so the
# build compiles it again. A PCH holds the absolute paths of the tree that compiled it; before ccache_launcher.sh kept
# PCH compiles out of base_dir, the shared cache could hand a tree another tree's Release PCH (S1's in LAND, 11-25:
# "redefinition of 'UUID'" -- the PCH's headers were S1's files). This tree's PCH names this tree about 30 KB
# in; another tree's never does. Only where ccache_launcher.sh compiles (not Windows).
dev_drop_foreign_pch() {
    [ "$DEV_HOST" = Windows ] && return 0
    local pch
    while IFS= read -r pch; do
        LC_ALL=C grep -q -a -F "$DEV_ROOT/" "$pch" && continue
        echo "dev: ${pch#"$DEV_BUILD"/} was compiled by another tree -- deleted, it is compiled again"
        rm -f "$pch" "$pch.sum"
    done < <(find "$DEV_BUILD" -path "*/$DEV_CONFIG/*" -name '*.pch' 2>/dev/null)
}

# dev_build <log> <target...> — build the DEV_CONFIG targets (and what they link) as one build of the machine's build
# pool (scripts/Dev/pool.sh run): the pool, not this script, decides how many compiles and links run at once, so builds
# of several trees run side by side. An inherited BUILDPOOL_CLASS names the pool.ini class of this build (pool.sh run
# keeps it when no --class is given).
dev_build() {
    local log="$1"; shift
    local args=() t
    for t in "$@"; do args+=(--target "$t"); done
    # The compile cache's settings are the build's own: buildpool_enable in CMakeLists.txt (Tools/BuildPool/cmake).
    dev_drop_foreign_pch >>"$log"
    bash "$DEV_ROOT/scripts/Dev/pool.sh" run --label "$(basename "$DEV_ROOT") ${args[*]}" --\
        cmake --build "$DEV_BUILD" --config "$DEV_CONFIG" "${args[@]}" >>"$log" 2>&1
}
