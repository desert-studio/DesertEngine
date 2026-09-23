#!/usr/bin/env bash
# Generate project files (premake5 gmake2) and build Desert Engine on macOS.
#
# Usage:
#   scripts/MacOS/BuildMacOS.sh [Debug|Release|Shipping] [--with-tests] [--gen-only] [--no-analyze]
#                               [--no-ccache]
#
# Examples:
#   scripts/MacOS/BuildMacOS.sh              # Debug build, static analysis on
#   scripts/MacOS/BuildMacOS.sh Release      # Release build
#   scripts/MacOS/BuildMacOS.sh Shipping     # what a player gets: no capture, no profiler, no counters
#   scripts/MacOS/BuildMacOS.sh Debug --with-tests
#   scripts/MacOS/BuildMacOS.sh Debug --no-analyze   # skip clang-tidy, and SAY so
#   scripts/MacOS/BuildMacOS.sh Debug --no-ccache    # compile every unit, and SAY so
set -euo pipefail

cd "$(dirname "$0")/../.."

CONFIG="Debug"
PREMAKE_ARGS=()
GEN_ONLY=0
ANALYZE=1
CCACHE=1

# WHERE THE ANALYSER ATTACHES. One path, named once. scripts/CI/CheckTidy.sh is the clang-tidy gate's
# single entry point and its exit codes ARE its interface — 0 clean, 1 findings, 2 the gate could not
# run — so this script never parses its output, only its status. The 1-versus-2 split is the reason
# the coupling is by exit code and not by "did it print anything": a gate that cannot run prints
# nothing, which is byte-identical to a gate that found nothing.
ANALYZER="scripts/CI/CheckTidy.sh"

for arg in "$@"; do
    case "$arg" in
        # Shipping is accepted here because the packager's own "Runtime binary not found" message tells
        # people to run `scripts/MacOS/BuildMacOS.sh Shipping` — advice a script that refused the word
        # would turn into a second dead end.
        Debug|Release|Shipping) CONFIG="$arg" ;;
        --with-tests)  PREMAKE_ARGS+=("--with-tests") ;;
        --gen-only)    GEN_ONLY=1 ;;
        --no-analyze)  ANALYZE=0 ;;
        --no-ccache)   CCACHE=0 ;;
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

# ---------------------------------------------------------------------------
# ccache.
#
# ── WHAT IT IS ACTUALLY FOR HERE, WITH THE MEASUREMENT ─────────────────────────────────────────────
#
# THE BUILD IS ~100 % COMPILING. Measured on this workspace (Debug + ASan, `make -j4`, 2246 compiler
# invocations logged one by one): 234 minutes of compiler CPU, 1.8 minutes of linking across 306 link
# invocations, and 17-20 s for `ar` over libDesert.a. The 2.7 GB static archive is not the cost and
# neither is the link — splitting it, or turning it into a shared library, buys back seconds.
#
# AND A THIRD OF THAT COMPILING IS THE SAME FILE, COMPILED AGAIN. The test suites do not link
# libDesert (exactly three makefiles in the workspace name it: Desert, Editor, Runtime); each suite
# LISTS the engine translation units it needs, deliberately, so that e.g. the asset layer keeps
# proving it compiles free of GPU types. The consequence is that 97 engine sources are compiled by
# more than one suite — Reflection.gen.cpp 39 times, ReflectionSerializer.cpp 35, SceneMigration.cpp
# 25 — with a byte-identical command line. Counted over the whole Debug build: 505 of 1940 compiles
# are a repeat of a (source, flags) pair already compiled in the SAME build, and they are 68.7 of the
# 223 minutes of attributable compile CPU, 30.8 %.
#
# That is why this is on by default and not only on CI: the first, largest win needs no persistence
# at all. A completely cold cache still turns those 505 repeats into hits inside one build.
#
# ── WHY NOT THE OTHER THINGS THAT WERE PRICED ──────────────────────────────────────────────────────
#
#   * Sharding the test job: every shard repays the whole build, so the makespan floor does not move.
#   * Splitting libDesert / linking tests against a shared object: 1.8 minutes of link CPU is the
#     entire prize, and no test binary links it in the first place.
#   * Dropping debug info (`-gline-tables-only`): real but small — measured on this machine over a
#     sample of translation units it cuts object size 54-60 % and compile time 6-14 %. It is a
#     separate decision because it changes what a developer can inspect, and it is not this one.
#
# ── WHAT MAKES A WRONG HIT IMPOSSIBLE ──────────────────────────────────────────────────────────────
#
# A cache that silently MISSES looks like a slow build. A cache that wrongly HITS looks like a passing
# build of code nobody compiled, which is strictly worse, so the settings below are argued one by one
# rather than copied from a blog post.
#
# ccache's direct mode keys an object on a hash of: the compiler binary, the full command line, the
# source file's contents, and the contents of EVERY header the previous compilation of that source
# opened (recorded in the cache's manifest). Change one byte of any of them and the key changes. So
# the only ways to a wrong hit are a 128-bit hash collision and the `sloppiness` list — which is why
# the list is spelled out here and kept to what the build actually needs:
#
#   pch_defines, time_macros — required, together, before ccache will cache ANY compilation that uses
#       a precompiled header, and Desert (271 units) and Editor both do. `time_macros` means a unit
#       expanding __DATE__/__TIME__/__TIMESTAMP__ could be served an object carrying an old date.
#       CENSUS, not assumption: zero occurrences of those three macros in Desert/, Editor/, Runtime/
#       and Tools/, and the only four files under ThirdParty/ that use them (optick's sample engine,
#       three dlib command-line tools) are compiled by no makefile in this workspace. If that census
#       ever stops being zero, this line is what has to be re-argued.
#
#   include_file_mtime, include_file_ctime — this build GENERATES headers and sources while it runs
#       (Version.gen.hpp from the Common project, Reflection.gen.cpp from DesertHeaderTool, the .gch
#       itself). Without these, ccache refuses to cache any unit that includes a file whose timestamp
#       is younger than the compilation, so the whole engine would sit uncached. What they switch off
#       is a timestamp heuristic guarding against a file being edited DURING its own compilation; the
#       content hash above is untouched and still decides every hit. make's own dependency ordering is
#       what makes that race unreachable here: the generators are prebuild steps, they finish before
#       any compile that reads their output starts.
#
# Nothing here is sloppy about the compiler itself: a different clang is a different hash, so a
# toolchain bump invalidates the cache rather than being served stale objects from the old one.
#
# ── AND IT SAYS WHAT IT DID ────────────────────────────────────────────────────────────────────────
#
# Every path prints a line. "ccache is not installed" must not look like "ccache worked", and the
# statistics are printed after the build so that a cache which has stopped hitting is visible in the
# log of the run where it stopped, not three weeks later in a duration graph.
# ---------------------------------------------------------------------------
MAKE_TOOL_VARS=()
if [ "$CCACHE" -eq 0 ]; then
    echo "!!! ccache DISABLED by --no-ccache: every translation unit will be compiled."
elif ! command -v ccache >/dev/null 2>&1; then
    echo "!!! ccache NOT FOUND: every translation unit will be compiled. Install it with"
    echo "!!!   brew install ccache        (or run scripts/MacOS/Setup.sh again)"
    CCACHE=0
else
    export CCACHE_SLOPPINESS="pch_defines,time_macros,include_file_mtime,include_file_ctime"
    export CCACHE_COMPRESS=1
    # ON THE make COMMAND LINE, NOT IN THE ENVIRONMENT, and the difference is load-bearing. premake's
    # generated makefiles assign the compiler behind `ifeq ($(origin CC), default)`, so a command-line
    # variable wins and is inherited by every sub-make through MAKEFLAGS. Exporting CC/CXX instead
    # would also reach scripts/CI/CheckTidy.sh below, which asks these same makefiles in dry run what
    # they would compile — and would hand it command lines beginning with `ccache`.
    MAKE_TOOL_VARS=(CC="ccache clang" CXX="ccache clang++")
    ccache --zero-stats >/dev/null
    echo "--- ccache ON: $(ccache --version | head -1), cache at $(ccache --get-config cache_dir)"
fi

# gmake2 configs are lowercase
MAKE_CONFIG="$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
CORES="$(sysctl -n hw.ncpu)"

echo "--- Building ($CONFIG, -j$CORES)"
make config="$MAKE_CONFIG" -j"$CORES" "${MAKE_TOOL_VARS[@]+"${MAKE_TOOL_VARS[@]}"}"

echo ""
echo "=== Build complete: build/Bin/$CONFIG ==="

if [ "$CCACHE" -eq 1 ]; then
    echo "--- ccache statistics for this build"
    ccache --show-stats
fi

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
