#!/usr/bin/env bash
# clang-tidy gate. Two modes:
#   scripts/CI/CheckTidy.sh [<base-ref-or-sha>]   changed lines only, against the merge-base
#   scripts/CI/CheckTidy.sh --all                 every translation unit in the workspace
#
# Exit codes, and they are the interface: 0 clean, 1 findings, 2 THE GATE COULD NOT RUN.
# 2 exists for the same reason it exists in CheckFormat.sh: this project's most frequent defect is an
# instrument that answers a different question with nothing in its output to say so. A missing
# compiler database, a missing analyser, or a changed-file list that came out empty when the diff did
# touch C++ all produce "no warnings" from clang-tidy, which reads exactly like a pass.
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD"

# THE VERSION IS PINNED TO 18, DELIBERATELY AND FOR THE SAME REASON THE FORMATTER IS.
# clang-tidy's check set, its fix-its and even which diagnostics exist change between releases:
# `misc-use-internal-linkage` is new in 18, `AnalyzeTemporaryDtors` was deleted in 16. An unpinned
# analyser gives the developer and CI different verdicts on identical code, which is the defect the
# formatter pin was introduced to close on 2026-09-08 — do not reopen it here.
# Homebrew's llvm@18 is already the documented developer toolchain for clang-format, so the analyser
# costs no new install: the same keg ships clang-tidy, run-clang-tidy and clang-tidy-diff.py.
TIDY=""
for candidate in "${DESERT_CLANG_TIDY:-}" clang-tidy-18 /opt/homebrew/opt/llvm@18/bin/clang-tidy; do
    [ -n "$candidate" ] || continue
    if command -v "$candidate" >/dev/null 2>&1; then TIDY="$candidate"; break; fi
done
if [ -z "$TIDY" ]; then
    echo "clang-tidy: MISSING TOOL. Looked for \$DESERT_CLANG_TIDY, clang-tidy-18," >&2
    echo "  /opt/homebrew/opt/llvm@18/bin/clang-tidy. On macOS: brew install llvm@18." >&2
    echo "This is an environment failure, NOT an analysis finding." >&2
    exit 2
fi
TIDY_VERSION="$("$TIDY" --version 2>&1 | tr -d '\n')"
case "$TIDY_VERSION" in
    *"version 18."*) ;;
    *)
        echo "clang-tidy: WRONG VERSION — '$TIDY_VERSION'." >&2
        echo "The gate is pinned to 18; another release enables and removes checks, so its verdict" >&2
        echo "is not comparable with CI's. Point \$DESERT_CLANG_TIDY at an 18.x binary." >&2
        exit 2
        ;;
esac
TIDY_DIR="$(dirname "$(command -v "$TIDY")")"
echo "clang-tidy gate using: $TIDY_VERSION"

# The compiler database is REGENERATED, never assumed. A stale one silently analyses the previous
# commit's flags, and a missing one makes clang-tidy fall back to bare defaults and drown in
# "'memory' file not found" — errors that WarningsAsErrors then reports as if they were findings.
if ! "$ROOT/scripts/CI/GenCompileCommands.sh" Debug; then
    echo "clang-tidy: could not build compile_commands.json — see above. Gate did not run." >&2
    exit 2
fi

# Homebrew's clang is not Xcode's, so it does not know where the macOS SDK is and cannot find
# <memory>. The build's own command line does not carry -isysroot because Apple's driver supplies it
# implicitly; this is the analyser's toolchain difference, not the build's, which is why it is added
# here and not baked into the database.
EXTRA=()
if [ "$(uname -s)" = "Darwin" ]; then
    SDK="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [ -z "$SDK" ]; then
        echo "clang-tidy: xcrun could not name a macOS SDK; the analyser cannot find libc++." >&2
        exit 2
    fi
    EXTRA+=(-extra-arg=-isysroot -extra-arg="$SDK")
fi

# A CPU COUNT THAT CAME OUT EMPTY MUST NOT BE PASSED ON AS ONE. Both probes are platform-specific —
# sysctl on macOS, nproc elsewhere — and on a stripped PATH neither resolves, leaving JOBS empty; the
# script then handed clang-tidy-diff.py `-j` with no value, argparse rejected it, and the whole gate
# came back as an environment failure for a reason that had nothing to do with the environment it was
# reporting on. Measured here on 2026-09-14. Four is a working default, and the substitution is said
# out loud rather than assumed.
JOBS="${TIDY_JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null) )}"
case "$JOBS" in
    ''|*[!0-9]*)
        echo "clang-tidy: could not read a CPU count (sysctl/nproc both unavailable); using -j4." >&2
        JOBS=4
        ;;
esac

# THE EXIT CODE IS THE VERDICT, AND IT COMES FROM .clang-tidy's `WarningsAsErrors: '*'`.
# clang-tidy exits 0 with findings unless warnings are errors, and run-clang-tidy and
# clang-tidy-diff.py both report the maximum child exit code — so without that setting this gate
# would be green over a wall of diagnostics. Nothing here re-asserts it: one place decides, and a
# developer running clang-tidy by hand gets the same verdict CI does.

if [ "${1:-}" = "--all" ]; then
    RCT="$(command -v run-clang-tidy-18 || echo "$TIDY_DIR/run-clang-tidy")"
    if [ ! -x "$RCT" ]; then
        echo "clang-tidy: run-clang-tidy not found beside $TIDY." >&2
        exit 2
    fi
    echo "--- whole workspace, -j$JOBS"
    "$RCT" -p "$ROOT" -clang-tidy-binary "$TIDY" -quiet -j "$JOBS" ${EXTRA[@]+"${EXTRA[@]}"}
    RC=$?
    [ "$RC" -eq 0 ] && echo "clang-tidy: clean over the whole workspace"
    exit $RC
fi

BASE_INPUT="${1:-origin/dev}"
if git rev-parse --verify -q "$BASE_INPUT^{commit}" >/dev/null 2>&1; then
    BASE=$(git merge-base HEAD "$BASE_INPUT" 2>/dev/null || echo "$BASE_INPUT")
else
    BASE=$(git rev-parse HEAD~1 2>/dev/null || git rev-parse HEAD)
fi

# WHY CHANGED LINES AND NOT THE WHOLE TREE: measured on this commit with `--all`, the check set in
# .clang-tidy reports 727 689 diagnostics over the 565 distinct translation units of the
# workspace (912 database entries; a source compiled by several test projects is one unit here), which
# collapse to 67 406 distinct source locations once the same header seen from many units is
# counted once. A whole-tree gate would be red on day one and stay red, so it would be turned off —
# which is how a config ends up in the repository that nobody runs, the exact state this task found
# .clang-tidy in. The format gate made the same call for the same reason and the tree has been
# converging under it ever since: code you TOUCH becomes clean.
# THE EXTENSION LIST IS DERIVED FROM WHAT THE BUILD ACTUALLY COMPILES, not from habit. The database
# holds .cpp AND .mm — Common/Platform/MacOS is Objective-C++ — and the tree carries two .h alongside
# its .hpp. An extension missing from this list is the quietest possible hole in the gate: the file
# changes, nothing matches, and the script prints "no C++ files changed" and exits 0.
CHANGED=$(git diff --name-only --diff-filter=ACMR "$BASE" \
          -- '*.cpp' '*.hpp' '*.mm' '*.h' | sed "s#^#$ROOT/#")
if [ -z "$CHANGED" ]; then
    echo "clang-tidy: no C++ files changed vs $BASE — nothing to analyse"
    exit 0
fi
N_CHANGED=$(printf '%s\n' "$CHANGED" | grep -c .)

# HOW MANY OF THE CHANGED FILES THIS GATE CAN ACTUALLY SEE. Counting the overlap with the database is
# what separates "this change has nothing to analyse" from "the database does not describe this tree
# and I am about to report a pass over zero files".
#
# A changed HEADER is deliberately NOT required to be in the database — it never can be. clang-tidy's
# InterpolatingCompilationDatabase infers a command for a file the database does not name from the
# nearest one it does, and for a header that is exactly right: measured on
# Editor/Source/Editor/Import/CookedJsonWrite.hpp — a header-only file no project compiles — it parsed
# the real translation unit and reported real diagnostics about our own code.
#
# A changed .cpp or .mm that is ABSENT IS FATAL, and this is the one place the same interpolation is a
# trap rather than a feature. Borrowing another project's flags gives the file another project's
# include paths, so a test source analysed with the engine's command line fails on `gtest/gtest.h file
# not found` — a clang-diagnostic-error, which WarningsAsErrors then reports with exit 1, i.e. as a
# finding about your code. That is precisely the instrument-answering-a-different-question shape this
# gate exists to catch, so absence is reported as an environment failure and named file by file.
# It happens for two reasons and the message names both: premake was not re-run after the file was
# added, or the build was generated without test projects while the change is in Desert/Tests.
COUNTS=$(CHANGED_FILES="$CHANGED" python3 -c '
import json, os
db = {e["file"] for e in json.load(open("compile_commands.json"))}
changed = os.environ["CHANGED_FILES"].split()
print(sum(1 for f in changed if f in db))
print(" ".join(f for f in changed if f.endswith((".cpp", ".mm")) and f not in db))')
COVERED=$(printf '%s\n' "$COUNTS" | sed -n 1p)
ORPHANS=$(printf '%s\n' "$COUNTS" | sed -n 2p)
echo "changed C++ files vs $BASE: $N_CHANGED, of which $COVERED are translation units in the database"
if [ -n "$ORPHANS" ]; then
    echo "clang-tidy: these changed sources are in NO premake project, so nothing compiles them and" >&2
    echo "the gate cannot know their flags:" >&2
    printf '    %s\n' $ORPHANS >&2
    echo "Re-generate with 'CI=true premake5 gmake2' — test projects exist only with CI set or" >&2
    echo "--with-tests. If a file is still absent afterwards, it is missing from a premake5.lua and" >&2
    echo "is not being built at all. This is an environment failure, NOT an analysis finding." >&2
    exit 2
fi

DIFFPY="$TIDY_DIR/../share/clang/clang-tidy-diff.py"
if [ ! -f "$DIFFPY" ]; then
    echo "clang-tidy: clang-tidy-diff.py not found at $DIFFPY (part of the llvm@18 keg)." >&2
    exit 2
fi

# -p1 because `git diff` prefixes a/ and b/. -W ignore silences Python 3.13+ SyntaxWarnings about
# LLVM 18's own unescaped regex literals, which are not ours to fix and bury the real output.
OUT=$(git diff -U0 "$BASE" -- '*.cpp' '*.hpp' '*.mm' '*.h' \
      | python3 -W ignore "$DIFFPY" -clang-tidy-binary "$TIDY" -p1 -path "$ROOT" -j "$JOBS" \
                -quiet ${EXTRA[@]+"${EXTRA[@]}"} 2>&1)
RC=$?
printf '%s\n' "$OUT"

# THREE OUTCOMES, NOT TWO, and the middle one was wrong here for a while. clang-tidy-diff.py returns
# the maximum exit code of the clang-tidy processes it ran — 0 clean, 1 diagnosed — but argparse
# failures and a crashed interpreter come back as 2 or higher, and an earlier version of this script
# reported those as "findings in the lines you changed". It did exactly what this gate exists to
# stop: an instrument answering a different question, with a confident message on top. Caught by the
# mutation test, which went red for the right code and the wrong reason.
case "$RC" in
    0)
        echo "clang-tidy: the lines you changed are clean (vs $BASE)"
        exit 0
        ;;
    1)
        echo ""
        echo "clang-tidy findings in the lines you changed. Reproduce locally with:"
        echo "  scripts/CI/CheckTidy.sh $BASE"
        exit 1
        ;;
    *)
        echo "" >&2
        echo "clang-tidy: the gate FAILED TO RUN (clang-tidy-diff.py exited $RC)." >&2
        echo "This is an environment failure, NOT an analysis finding — the output above is the" >&2
        echo "tool's own complaint, not a diagnosis of your code." >&2
        exit 2
        ;;
esac
