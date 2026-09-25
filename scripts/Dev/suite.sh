#!/usr/bin/env bash
# suite.sh <Suite…> — build (Debug) and run the named test suites, from the tree root, in one call.
# Builds with ~/.claude/tools/build_quiet.sh (waits for any other make, ccache, -j4) or plain make -f <Suite>.make.
# Prints one line per suite: `Suite: N passed / M failed` plus up to 5 failed test names; exit 1 on any red.
# The .make files exist only after `CI=true premake5 gmake`. Logs: build/DevLogs/suite-*/.
# Each run is capped at 300 s (SUITE_TIMEOUT overrides).
set -u
source "$(dirname "$0")/_common.sh"
case "${1:-}" in -h|--help|"") dev_help "$0"; exit 0 ;; esac
build_only=""; [ "${1:-}" = --build-only ] && { build_only=1; shift; }  # handoff_check builds first, runs itself
cd "$DEV_ROOT" || exit 2
LOG=$(dev_logdir suite)
dev_regen_makefiles "$LOG" || exit 2
for s in "$@"; do [ -f "$s.make" ] || { echo "suite.sh: $s.make not found (CI=true premake5 gmake?)"; exit 2; }; done
QUIET="$HOME/.claude/tools/build_quiet.sh"
if [ -x "$QUIET" ]; then
    # build_quiet drives the root Makefile, whose per-suite targets also rebuild what the suite links.
    out=$("$QUIET" "$DEV_ROOT" "$LOG/build.log" "$@") || { echo "$out" | head -10; exit 2; }
else
    while pgrep -x make >/dev/null; do sleep 10; done
    export CCACHE_SLOPPINESS="pch_defines,time_macros,include_file_mtime,include_file_ctime" CCACHE_COMPRESS=1 CCACHE_BASEDIR=/Users/daniilsavcenko/Desktop/Programming/C++
    for s in "$@"; do
        make -f "$s.make" config=debug -j4 CC="ccache clang" CXX="ccache clang++" >>"$LOG/build.log" 2>&1 ||
            { echo "suite.sh: build of $s FAILED; log $LOG/build.log"; grep -m 8 -E "error:|Undefined symbols|No rule" "$LOG/build.log"; exit 2; }
    done
fi
[ -n "$build_only" ] && { echo "suite.sh: built $# suite(s)"; exit 0; }
fail=0
for s in "$@"; do
    dev_capped "${SUITE_TIMEOUT:-300}" "build/Bin/Tests/Debug/$s" </dev/null >"$LOG/$s.log" 2>&1
    rc=$?
    p=$(grep -a -c '^\[       OK \]' "$LOG/$s.log")
    f=$(grep -a '^\[  FAILED  \] [A-Za-z_].*(' "$LOG/$s.log" | sort -u | wc -l | tr -d ' ')
    extra=""; [ $rc -ne 0 ] && [ "$f" = 0 ] && extra=" (rc=$rc$([ $rc = 124 ] && echo ", TIMEOUT"))"
    echo "$s: $p passed / $f failed$extra"
    [ $rc -ne 0 ] && fail=1
    grep -a '^\[  FAILED  \] [A-Za-z_].*(' "$LOG/$s.log" | sed 's/^\[  FAILED  \] /  /; s/ (.*//' | sort -u | head -5
done
echo "logs $LOG"
exit $fail
