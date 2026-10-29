#!/usr/bin/env bash
# suite.sh <Suite…> — build (Debug) and run the named test suites, from the tree root, in one call.
# A suite's executable comes from build/TestManifest.txt (`<Executable> <Suite>`): its layer's runner
# (CommonTests, ToolsTests, ...), run as `<runner> --desert-suite=<Suite>`, or a not-yet-converted suite's own binary.
# Configures the host preset if needed (_common.sh dev_configure), then builds the runners with cmake --build (Debug,
# compiler cache, -j4 or DEV_JOBS, after any other ninja finishes). Logs: build/DevLogs/suite-*/.
# Prints one line per suite: `Suite: N passed / M failed` plus up to 5 failed test names; exit 1 on any red.
# Each run is capped at 300 s (SUITE_TIMEOUT overrides).
# --dev-suites: build ONE runner, DevTests, of only the named suites (DESERT_DEV_SUITES) instead of their layers'
# runners: no other suite's TU is compiled and there is one link. The CI runners are untouched.
set -u
source "$(dirname "$0")/_common.sh"
case "${1:-}" in -h|--help|"") dev_help "$0"; exit 0 ;; esac
build_only=""; dev=""
while :; do
    case "${1:-}" in
        --build-only) build_only=1; shift ;;  # handoff_check builds first, runs itself
        --dev-suites) dev=1; shift ;;
        *) break ;;
    esac
done
[ $# -gt 0 ] || { echo "suite.sh: no suite named"; exit 2; }
cd "$DEV_ROOT" || exit 2
LOG=$(dev_logdir suite)
MANIFEST="$DEV_ROOT/build/TestManifest.txt"
if [ -n "$dev" ]; then
    dev_configure "$LOG" "$(IFS=,; echo "$*")" || exit 2
    exe_of() { awk -v s="$1" '$2 == s { print "DevTests" }' "$MANIFEST"; }
else
    dev_configure "$LOG" || exit 2
    exe_of() { awk -v s="$1" '$2 == s { print $1 }' "$MANIFEST"; }
fi
targets=()
for s in "$@"; do
    e=$(exe_of "$s")
    [ -n "$e" ] || { echo "suite.sh: $s is not in build/TestManifest.txt (no directory Desert/Tests/<Layer>/$s?)"; exit 2; }
    dev_has_target "$e" || { echo "suite.sh: the build defines no target $e (build/cmake/$DEV_PRESET)"; exit 2; }
    case " ${targets[*]-} " in *" $e "*) ;; *) targets+=("$e") ;; esac
done
dev_build "$LOG/build.log" "${targets[@]}" ||
    { echo "suite.sh: build of ${targets[*]} FAILED; log $LOG/build.log"; grep -m 8 -E "error|Undefined symbols" "$LOG/build.log"; exit 2; }
[ -n "$build_only" ] && { echo "suite.sh: built ${#targets[@]} executable(s) for $# suite(s)"; exit 0; }
fail=0
for s in "$@"; do
    dev_capped "${SUITE_TIMEOUT:-300}" "build/Bin/Tests/Debug/$(exe_of "$s")" --desert-suite="$s" </dev/null >"$LOG/$s.log" 2>&1
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
