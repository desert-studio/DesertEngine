#!/usr/bin/env bash
# Generate project files (premake5 gmake2) and build Desert Engine on macOS.
#
# Usage:
#   scripts/MacOS/BuildMacOS.sh [Debug|Release] [--with-tests] [--gen-only] [--no-analyze]
#
# Examples:
#   scripts/MacOS/BuildMacOS.sh              # Debug build, static analysis on
#   scripts/MacOS/BuildMacOS.sh Release      # Release build
#   scripts/MacOS/BuildMacOS.sh Debug --with-tests
#   scripts/MacOS/BuildMacOS.sh Debug --no-analyze   # skip clang-tidy, and SAY so
set -euo pipefail

cd "$(dirname "$0")/../.."

CONFIG="Debug"
PREMAKE_ARGS=()
GEN_ONLY=0
ANALYZE=1

# WHERE THE ANALYSER ATTACHES. One path, named once. scripts/CI/CheckTidy.sh is the clang-tidy gate's
# single entry point and its exit codes ARE its interface — 0 clean, 1 findings, 2 the gate could not
# run — so this script never parses its output, only its status. The 1-versus-2 split is the reason
# the coupling is by exit code and not by "did it print anything": a gate that cannot run prints
# nothing, which is byte-identical to a gate that found nothing.
ANALYZER="scripts/CI/CheckTidy.sh"

for arg in "$@"; do
    case "$arg" in
        Debug|Release) CONFIG="$arg" ;;
        --with-tests)  PREMAKE_ARGS+=("--with-tests") ;;
        --gen-only)    GEN_ONLY=1 ;;
        --no-analyze)  ANALYZE=0 ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

if ! command -v premake5 >/dev/null 2>&1; then
    echo "premake5 not found. Run scripts/MacOS/Setup.sh first." >&2
    exit 1
fi

# NO GenVersion.sh CALL HERE, AND THE ABSENCE IS THE FIX. This script's single call to it was the
# ONLY one in the repository, so the build identity was refreshed for whoever went through this
# wrapper and for nobody else — `make` on its own compiled a header a month out of date. The generator
# is now part of the Common project's own makefile, which the `make` below runs anyway. Calling it
# here as well would put the refresh in two places, and the one that runs first would hide a hook that
# had stopped working: a missing header is a compile error, a stale one is a lie.

echo "--- Generating Makefiles (premake5 gmake2)"
premake5 gmake2 "${PREMAKE_ARGS[@]+"${PREMAKE_ARGS[@]}"}"

if [ "$GEN_ONLY" -eq 1 ]; then
    exit 0
fi

# gmake2 configs are lowercase
MAKE_CONFIG="$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
CORES="$(sysctl -n hw.ncpu)"

echo "--- Building ($CONFIG, -j$CORES)"
make config="$MAKE_CONFIG" -j"$CORES"

echo ""
echo "=== Build complete: build/Bin/$CONFIG ==="

# ---------------------------------------------------------------------------
# Static analysis.
#
# THE ONLY OUTCOME THAT MAY BE SILENT IS THE ONE WHERE IT RAN. Every other path prints a line saying
# the analyser did not run and why, because a check that is skipped without saying so is worse than
# one that was never added: the build looks exactly as it does when the code is clean, so the absence
# is invisible for as long as nobody goes looking. That is the same shape as the test runner that
# could not report a failure on Windows for months, and as the format gate that reported "could not
# run" as violations.
#
# NOT UNDER CI, and this is a decision with a number behind it rather than a convenience. The macOS
# `sanitizers` job measured 55-66 minutes against a 90-minute ceiling (61-73 %), and it calls this
# script; adding a whole-workspace analysis pass to it buys nothing, because a gate belongs in its own
# job where its verdict is separable from the build's, exactly as the clang-format gate already is.
# A timed-out job reports as `cancelled`, which looks like nothing being wrong.
# ---------------------------------------------------------------------------
if [ "$ANALYZE" -eq 0 ]; then
    echo "!!! static analysis SKIPPED: --no-analyze was passed. Nothing was checked by clang-tidy."
elif [ -n "${CI:-}" ]; then
    echo "!!! static analysis SKIPPED: running under CI, where the clang-tidy gate is its own job."
elif [ ! -x "$ANALYZER" ]; then
    # A REFUSAL, NOT A SHRUG. The analyser is on by default, so "it is on" and "it quietly did not
    # happen" cannot both be true — and this branch is reachable only in the window before the gate
    # itself lands, which is precisely when a shrug would set the habit of ignoring it.
    echo "" >&2
    echo "BUILD OK, BUT THE ANALYSER DID NOT RUN: $ANALYZER is missing or not executable." >&2
    echo "  Static analysis is on by default, so this is a refusal rather than a skip." >&2
    echo "  Build without it deliberately:  scripts/MacOS/BuildMacOS.sh $CONFIG --no-analyze" >&2
    exit 1
else
    echo "--- Static analysis (clang-tidy, changed lines)"
    set +e
    "$ANALYZER"
    ANALYZER_RC=$?
    set -e
    case "$ANALYZER_RC" in
        0) echo "=== clang-tidy: clean ===" ;;
        1)
            echo "clang-tidy reported findings in the lines you changed (see above)." >&2
            exit 1
            ;;
        *)
            # Exit 2 and anything unexpected are the same thing to this script: the gate did not
            # produce a verdict, and "no verdict" must never be reported as "clean".
            echo "clang-tidy COULD NOT RUN (exit $ANALYZER_RC) — this is an environment failure," >&2
            echo "  not a clean analysis. The binaries in build/Bin/$CONFIG are still valid." >&2
            exit 1
            ;;
    esac
fi
