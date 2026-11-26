#!/usr/bin/env bash
# Shared by scripts/Dev/*.sh: the tree root, a per-run log directory, and a per-test time cap.
# Logs live under build/DevLogs (ignored by git, one copy per worktree) so parallel agents never share them.

DEV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The host's configure preset (CMakePresets.json) and its build tree. Single-config Ninja: one tree per configuration
# (build/cmake/<preset>). DEV_CONFIG is the host's row of scripts/Dev/dev_config.txt (play_scenario.py reads the same
# row) and names the preset: Development -> <host>-dev, DebugGame -> <host>-debug, Shipping -> <host>-ship.
# A host without a row, or a row naming another configuration, is an error, not a default.
case "$(uname -s)" in
    Darwin) DEV_PREFIX="mac"; DEV_HOST=Darwin ;;
    *)      DEV_PREFIX="win"; DEV_HOST=Windows ;;
esac
DEV_CONFIG="$(awk -v host="$DEV_HOST" '$1 == host { print $2 }' "$DEV_ROOT/scripts/Dev/dev_config.txt" 2>/dev/null)"
[ -n "$DEV_CONFIG" ] || { echo "_common.sh: scripts/Dev/dev_config.txt has no row for $DEV_HOST" >&2; return 1 2>/dev/null || exit 1; }
case "$DEV_CONFIG" in
    Development) DEV_PRESET="$DEV_PREFIX-dev" ;;
    DebugGame)   DEV_PRESET="$DEV_PREFIX-debug" ;;
    Shipping)    DEV_PRESET="$DEV_PREFIX-ship" ;;
    *) echo "_common.sh: dev_config.txt names '$DEV_CONFIG'; the configurations are Development, DebugGame, Shipping" >&2
       return 1 2>/dev/null || exit 1 ;;
esac
DEV_BUILD="$DEV_ROOT/build/cmake/$DEV_PRESET"
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
# A merge that adds or removes sources needs nothing here: it changes a committed Sources.cmake (sources_sync.py), and
# the next build re-configures by itself because that list changed. The suite list (suite.sh --dev-suites) is the one input a build cannot see,
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
    grep -q "^build $1: phony" "$DEV_BUILD/build.ninja" 2>/dev/null
}

# dev_up_to_date <name> — Ninja, asked in dry run, has nothing left to do for the target: it is newer than every object,
# library and generated file it depends on. The build's own dependency graph answers, not a timestamp guess.
dev_up_to_date() {
    # The engine reads committed source lists and its generators run only on changed inputs (B-TREE), so a dry run of
    # an up-to-date engine target says "no work to do". The DevTests suites still glob (CONFIGURE_DEPENDS): with them
    # configured, the glob re-check and the CMake re-run behind it are listed after every build and are not work.
    local out
    out=$(ninja -C "$DEV_BUILD" -n "$1" 2>/dev/null) || return 1
    grep -q '^ninja: no work to do' <<<"$out" && return 0
    ! grep -E '^\[[0-9]+/[0-9]+\] ' <<<"$out" | grep -vqE '\] (Re-checking globbed directories|Re-running CMake)\.\.\.$'
}

# dev_drop_foreign_pch — deletes every precompiled header of this tree that another tree compiled, so the
# build compiles it again. A PCH holds the absolute paths of the tree that compiled it; before ccache_launcher.sh kept
# PCH compiles out of base_dir, the shared cache could hand a tree another tree's PCH (S1's in LAND, 11-25:
# "redefinition of 'UUID'" -- the PCH's headers were S1's files). This tree's PCH names this tree about 30 KB
# in; another tree's never does. Only where ccache_launcher.sh compiles (not Windows).
dev_drop_foreign_pch() {
    [ "$DEV_HOST" = Windows ] && return 0
    local pch
    while IFS= read -r pch; do
        LC_ALL=C grep -q -a -F "$DEV_ROOT/" "$pch" && continue
        echo "dev: ${pch#"$DEV_BUILD"/} was compiled by another tree -- deleted, it is compiled again"
        rm -f "$pch" "$pch.sum"
    done < <(find "$DEV_BUILD" -name '*.pch' 2>/dev/null)
}

# dev_build <log> <target...> — build the DEV_CONFIG targets (and what they link) with the compiler cache, after any other
# build on this machine finished: two parallel builds share the CPU and both slow down by more than they gain.
dev_build() {
    local log="$1"; shift
    local args=() t
    for t in "$@"; do args+=(--target "$t"); done
    if command -v pgrep >/dev/null 2>&1; then
        while pgrep -x ninja >/dev/null; do sleep 10; done
    fi
    # The compile cache's settings (base_dir, PCH digests) are the build's own: scripts/Dev/ccache_launcher.sh.
    dev_drop_foreign_pch >>"$log"
    # The branch's own changed sources leave the unity batches (adaptive unity); a changed list reconfigures.
    python "$DEV_ROOT/scripts/Dev/adaptive_unity.py" "$DEV_BUILD" >>"$log" 2>&1 || return 1
    cmake --build "$DEV_BUILD" -j "${DEV_JOBS:-4}" "${args[@]}" >>"$log" 2>&1
}
