#!/usr/bin/env bash
# Shared by scripts/Dev/*.sh: the tree root, a per-run log directory, and a per-test time cap.
# Logs live under build/DevLogs (ignored by git, one copy per worktree) so parallel agents never share them.

DEV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The host's configure preset (CMakePresets.json) and its build tree. Ninja Multi-Config: one tree holds Debug, Release
# and Shipping, each with its own build-<Config>.ninja; the dev scripts build and run Debug.
case "$(uname -s)" in
    Darwin) DEV_PRESET="macos-clang-ninja" ;;
    *)      DEV_PRESET="windows-msvc-ninja" ;;
esac
DEV_BUILD="$DEV_ROOT/build/cmake/$DEV_PRESET"

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

# dev_has_target <name> — the Debug build defines that target (a runner, a tool, Editor).
dev_has_target() {
    grep -q "^build $1: phony" "$DEV_BUILD/build-Debug.ninja" 2>/dev/null
}

# dev_up_to_date <name> — Ninja, asked in dry run, has nothing left to do for the target: it is newer than every object,
# library and generated file it depends on. The build's own dependency graph answers, not a timestamp guess.
dev_up_to_date() {
    ninja -C "$DEV_BUILD" -f build-Debug.ninja -n "$1" 2>/dev/null | grep -q '^ninja: no work to do'
}

# dev_build <log> <target...> — build the Debug targets (and what they link) with the compiler cache, after any other
# build on this machine finished: two parallel builds share the CPU and both slow down by more than they gain.
dev_build() {
    local log="$1"; shift
    local args=() t
    for t in "$@"; do args+=(--target "$t"); done
    if command -v pgrep >/dev/null 2>&1; then
        while pgrep -x ninja >/dev/null; do sleep 10; done
    fi
    # BASEDIR = the directory holding every worktree, so two trees' identical compiles share one cache entry.
    export CCACHE_SLOPPINESS="pch_defines,time_macros,include_file_mtime,include_file_ctime" CCACHE_COMPRESS=1
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$(dirname "$DEV_ROOT")}"
    cmake --build "$DEV_BUILD" --config Debug -j "${DEV_JOBS:-4}" "${args[@]}" >>"$log" 2>&1
}
