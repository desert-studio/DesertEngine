#!/usr/bin/env bash
# Run every built test binary. Unix counterpart of scripts/Windows/RunTests.ps1.
#
# Called by ci.yml's "Run tests" steps, and by hand. NOT by a postbuild: the RunAllTests project used
# to list this script in `postbuildcommands` and that line never executed once — see the note in
# Desert/Tests/premake5.lua where it was removed.
#
# THIS RUNNER GLOBS; THE WINDOWS ONE DOES NOT. It runs whatever executables it finds in $TEST_DIR,
# so a suite that stopped LINKING is silently not run rather than reported. The Windows side reads
# build/TestManifest.txt — the list of suites premake generated projects for — and fails on a name
# with no binary behind it. Closing the gap here means deciding what a manifest entry means on a
# platform where a suite may legitimately not be built, which is a separate change with its own
# verdict; until then this asymmetry is a known hole and not an oversight. What this runner DOES do
# is compare the two counts and say so (see "manifest" below) — not a gate, but the silence is gone,
# and it is load-bearing for the timings: a suite that stops linking makes the sweep FASTER, and a
# faster sweep with no explanation is indistinguishable from an optimisation that worked.
#
# ── WHY THIS RUNS SUITES CONCURRENTLY, WITH THE NUMBERS ────────────────────────────────────────────
#
# It used to run them one after another. Measured on CI (job 106994488524, macos-14, ASan+UBSan,
# `make -j3`): the test phase was 40.1 minutes of wall clock on a THREE-core runner with two of those
# cores idle throughout. The distribution is why that mattered — 279 suites, median 0.18 s, 237 of
# them under 5 s, and the top ten accounting for 71.7 % of the 40 minutes (CloudField alone 496 s,
# 20.7 %). Four independent CI runs the same night agreed on that shape to within a percent.
#
# A serial sweep over a distribution like that spends most of its wall clock waiting on a handful of
# suites while the machine is idle. What concurrency actually bought, MEASURED and not modelled (job
# 107030112391, the same workspace, three jobs on three cores): 30 m 16 s of makespan where the serial
# sweep on a runner of that speed would have been about 48.7 minutes. The job went 78 minutes instead
# of the ~96 it was heading for, and passed.
#
# THE MODEL THAT PREDICTED 13 MINUTES WAS WRONG, and the number it was wrong by is the useful part.
# `max(longest suite, total / JOBS)` assumes a suite costs the same whether or not two others are
# running beside it. It does not: the SUM of the per-suite durations went from 2403 s serial to 5416 s
# under three-way concurrency, 2.25x, and the worst offenders inflated most (CloudNoiseVolume 3.33x,
# CloudPlacementSpectrum 3.34x, CloudField 2.38x). These suites are compute-bound and three of them on
# three cores saturate the machine, so the real speedup is 1.6x, not 3x. Raising JOBS past the core
# count would buy nothing; there is no idle left to sell.
#
# WHICH MOVES THE BINDING CONSTRAINT. CloudField alone is 1182 s under concurrency — 65 % of the whole
# makespan — so the next minute has to come out of that suite, out of the build, or out of a second
# machine. This is not a fix for a pathological suite and does not pretend to be one: the per-suite
# table below is what makes the next one visible before it becomes a ceiling, and it is what named
# this one.
#
# MEMORY IS NOT THE LIMIT, which was the open question when this was written: peak RSS over the whole
# concurrent sweep was 1122 MiB, in SceneForeignKeys, against a 7 GB runner.
#
# THE SCHEDULE IS FIFO over the alphabetical glob, deliberately, because every alternative needs
# state that rots — a committed weights table, or the previous run's timings restored from an
# artifact. The cost of FIFO is that a long suite drawn late extends the makespan past total/JOBS, so
# the summary reports the makespan AND the critical suite: if ordering ever becomes the binding
# constraint, the run says so rather than being inferred.
#
# JOBS defaults to the core count. Override with TEST_JOBS=1 to get the old serial behaviour back —
# which is also how you read the output of a suite that only misbehaves under concurrency.
#
# ── WHAT IS PRINTED, AND WHY NOT EVERYTHING ────────────────────────────────────────────────────────
#
# Concurrent writers interleave, so each suite's output is captured to build/TestReports/logs/<name>.log
# and replayed by the parent in a fixed order. A PASSING suite is one line (name, duration, peak RSS);
# a FAILING suite has its whole captured log reproduced. The verdict never comes from what was
# printed: every worker writes a status file, and this script fails if any status is non-zero, if any
# status file is MISSING (a worker killed by the OOM killer writes nothing — that is the one failure
# mode a "no output means success" runner cannot see), or if xargs itself reported a failure.
#
# Usage: scripts/MacOS/RunTests.sh <workspace-root> <config>
#        TEST_JOBS=<n> scripts/MacOS/RunTests.sh <workspace-root> <config>
set -uo pipefail

# ── worker mode ───────────────────────────────────────────────────────────────────────────────────
# Re-entry point invoked by xargs, one process per suite: `RunTests.sh --one <root> <config> <name>`.
# It is a mode of this script rather than a second file so that the two halves cannot drift apart.
if [ "${1:-}" = "--one" ]; then
    shift
    W_ROOT="$1"
    W_CONFIG="$2"
    W_NAME="$3"
    W_BIN="$W_ROOT/build/Bin/Tests/$W_CONFIG/$W_NAME"
    W_REPORTS="$W_ROOT/build/TestReports"
    W_LOG="$W_REPORTS/logs/$W_NAME.log"
    W_STATUS="$W_REPORTS/status/$W_NAME"

    W_START="$(date +%s)"
    # /usr/bin/time -l is BSD time's verbose form and prints "maximum resident set size" in bytes.
    # It is the cheapest honest answer to "can this many of these run at once", which is the question
    # concurrency raises on a sanitizer build. Guarded because a Unix without it is still a Unix.
    if [ -x /usr/bin/time ]; then
        /usr/bin/time -l "$W_BIN" --gtest_output="xml:$W_REPORTS/$W_NAME.xml" >"$W_LOG" 2>&1
        W_RC=$?
    else
        "$W_BIN" --gtest_output="xml:$W_REPORTS/$W_NAME.xml" >"$W_LOG" 2>&1
        W_RC=$?
    fi
    W_END="$(date +%s)"

    W_RSS="$(awk '/maximum resident set size/ { print $1 }' "$W_LOG" | tail -1)"
    case "$W_RSS" in
    '' | *[!0-9]*) W_RSS=0 ;;
    esac

    echo "$W_RC $((W_END - W_START)) $W_RSS" >"$W_STATUS"
    exit "$W_RC"
fi

# ── driver ────────────────────────────────────────────────────────────────────────────────────────
ROOT="${1:?workspace root required}"
CONFIG="${2:-Debug}"

TEST_DIR="$ROOT/build/Bin/Tests/$CONFIG"
REPORT_DIR="$ROOT/build/TestReports"
LOG_DIR="$REPORT_DIR/logs"
STATUS_DIR="$REPORT_DIR/status"
MANIFEST="$ROOT/build/TestManifest.txt"

SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

if [ ! -d "$TEST_DIR" ]; then
    echo "[ERROR] test dir not found: $TEST_DIR"
    exit 1
fi

JOBS="${TEST_JOBS:-}"
if [ -z "$JOBS" ]; then
    JOBS="$( (sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1) | head -1)"
fi
case "$JOBS" in
'' | *[!0-9]*) JOBS=1 ;;
esac
[ "$JOBS" -lt 1 ] && JOBS=1

# A stale status file from a previous sweep would be read as this sweep's verdict, so both scratch
# directories are rebuilt rather than reused. The .xml reports are left alone — the CI artifact step
# uploads that directory and an old report is at worst old, not a wrong answer about this run.
rm -rf "$LOG_DIR" "$STATUS_DIR"
mkdir -p "$REPORT_DIR" "$LOG_DIR" "$STATUS_DIR"

NAMES=""
COUNT=0
for test_bin in "$TEST_DIR"/*; do
    [ -f "$test_bin" ] && [ -x "$test_bin" ] || continue
    NAMES="$NAMES$(basename "$test_bin")
"
    COUNT=$((COUNT + 1))
done

if [ "$COUNT" -eq 0 ]; then
    echo "[ERROR] no test binaries in $TEST_DIR"
    exit 1
fi

echo "===== Starting Tests ====="
echo "suites: $COUNT   jobs: $JOBS   dir: $TEST_DIR"

# -0 and not newline-delimited: xargs splits on whitespace by default, and this repository's own
# checkout path contains a `+`, a space away from a path that would be split in half silently.
QUEUE="$REPORT_DIR/queue.nul"
printf '%s\n' "$NAMES" | grep -v '^$' | tr '\n' '\0' >"$QUEUE"

SWEEP_START="$(date +%s)"
# Fed from a file rather than a pipe: with `pipefail` set, a pipeline's status is not necessarily
# xargs's own, and the verdict must not depend on which stage of a pipe answered.
xargs -0 -n1 -P "$JOBS" "$SELF" --one "$ROOT" "$CONFIG" <"$QUEUE"
XARGS_RC=$?
SWEEP_END="$(date +%s)"
MAKESPAN=$((SWEEP_END - SWEEP_START))

echo "===== Test Results ====="

ERROR=0
FAILED=""
MISSING=""
TOTAL_SEC=0
CRIT_SEC=0
CRIT_NAME=""
PEAK_RSS=0
PEAK_RSS_NAME=""
RAN=0

for name in $NAMES; do
    status_file="$STATUS_DIR/$name"
    if [ ! -f "$status_file" ]; then
        # No status file means the worker never reached its last line: killed, OOM, or crashed the
        # shell. Silence here used to read as success; it reads as a failure now.
        MISSING="$MISSING $name"
        ERROR=1
        continue
    fi
    rc=""; secs=""; rss=""
    read -r rc secs rss <"$status_file"
    case "$rc$secs$rss" in '' | *[!0-9]*)
        MISSING="$MISSING $name(unreadable-status)"
        ERROR=1
        continue ;;
    esac
    RAN=$((RAN + 1))
    TOTAL_SEC=$((TOTAL_SEC + secs))
    if [ "$secs" -ge "$CRIT_SEC" ] && [ -z "$CRIT_NAME" -o "$secs" -gt "$CRIT_SEC" ]; then
        CRIT_SEC="$secs"
        CRIT_NAME="$name"
    fi
    if [ "$rss" -gt "$PEAK_RSS" ]; then
        PEAK_RSS="$rss"
        PEAK_RSS_NAME="$name"
    fi
    if [ "$rc" -ne 0 ]; then
        FAILED="$FAILED $name"
        ERROR=1
        echo "[TEST] $name"
        # The whole captured log, minus the resource block /usr/bin/time appends, which is noise
        # next to a failing assertion.
        sed '/maximum resident set size/,$d' "$LOG_DIR/$name.log"
        echo "[FAIL] $name (exit $rc, ${secs}s)"
    else
        echo "[TEST] $name  ${secs}s  $((rss / 1048576)) MiB"
    fi
done

echo
echo "----- Timing -----"
echo "makespan     : ${MAKESPAN}s ($((MAKESPAN / 60))m$((MAKESPAN % 60))s) over $JOBS jobs"
echo "suite-seconds: ${TOTAL_SEC}s (serial equivalent $((TOTAL_SEC / 60))m$((TOTAL_SEC % 60))s)"
echo "critical     : $CRIT_NAME at ${CRIT_SEC}s — the makespan cannot go below this"
[ "$PEAK_RSS" -gt 0 ] && echo "peak RSS     : $((PEAK_RSS / 1048576)) MiB in $PEAK_RSS_NAME"
echo
echo "slowest 15:"
for name in $NAMES; do
    [ -f "$STATUS_DIR/$name" ] || continue
    rc=""; secs=""; rss=""
    read -r rc secs rss <"$STATUS_DIR/$name"
    case "$secs" in '' | *[!0-9]*) continue ;; esac
    echo "$secs $name"
done | sort -rn | head -15 | awk '{ printf "  %6ss  %s\n", $1, $2 }'
echo

# The manifest cross-check. NOT a gate — see the header — but the counts are printed on every run so
# that a suite quietly leaving the sweep is a visible event rather than a faster job.
if [ -f "$MANIFEST" ]; then
    EXPECTED="$(grep -c '[^[:space:]]' "$MANIFEST")"
    echo "suites run: $RAN of $COUNT found ($EXPECTED in build/TestManifest.txt)"
    if [ "$COUNT" -ne "$EXPECTED" ]; then
        MSG="$COUNT binaries in $TEST_DIR but $EXPECTED projects in build/TestManifest.txt — a suite is not linking, or the manifest is stale"
        # The ::workflow command:: form is an annotation on CI and line noise in a terminal, so the
        # same fact is said in whichever dialect the reader is actually in.
        if [ -n "${GITHUB_ACTIONS:-}" ]; then
            echo "::warning title=Test suite count::$MSG"
        else
            echo "[WARN] $MSG"
        fi
    fi
else
    echo "suites run: $RAN of $COUNT found (no build/TestManifest.txt to compare against)"
fi

if [ -n "$MISSING" ]; then
    echo "[ERROR] no result recorded for:$MISSING"
fi
if [ -n "$FAILED" ]; then
    echo "[FAIL] failing suites:$FAILED"
fi
# xargs's own verdict is kept as a third opinion: it catches a worker that could not be started at
# all, which leaves neither a status file this loop can miss nor a name this loop knows to look for.
if [ "$XARGS_RC" -ne 0 ] && [ "$ERROR" -eq 0 ]; then
    echo "[ERROR] xargs reported failure ($XARGS_RC) but every suite recorded success — treating as a failure"
    ERROR=1
fi

if [ "$ERROR" -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "SOME TESTS FAILED"
fi
exit "$ERROR"
