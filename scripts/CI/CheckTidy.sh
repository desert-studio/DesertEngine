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
    EXTRA+=(--extra-arg=-isysroot --extra-arg="$SDK")
fi

JOBS="${TIDY_JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )}"

# THE EXIT CODE COMES FROM HERE, NOT FROM .clang-tidy, and the split is deliberate. clang-tidy exits
# 0 with findings unless warnings are errors, so both run-clang-tidy and clang-tidy-diff.py — which
# report the maximum child exit code — return 0 over a wall of diagnostics. A gate that read the
# output text instead would be a gate that passes when the tool crashes.
# It is NOT in .clang-tidy because that file is also what a developer's editor and a bare
# `clang-tidy Foo.cpp` read: with '*' there, the first clang-diagnostic-* finding becomes an error,
# and clang stops at its error limit, so the listing you are reading gets truncated exactly when it
# is longest. Blocking is the gate's job; describing is the config's.
WAE=(-warnings-as-errors=*)

if [ "${1:-}" = "--all" ]; then
    RCT="$(command -v run-clang-tidy-18 || echo "$TIDY_DIR/run-clang-tidy")"
    if [ ! -x "$RCT" ]; then
        echo "clang-tidy: run-clang-tidy not found beside $TIDY." >&2
        exit 2
    fi
    echo "--- whole workspace, -j$JOBS"
    "$RCT" -p "$ROOT" -clang-tidy-binary "$TIDY" -quiet -j "$JOBS" "${EXTRA[@]}"
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

# WHY CHANGED LINES AND NOT THE WHOLE TREE: measured on this commit, the owner's check set reports
# five figures of findings over 902 translation units. A whole-tree gate would be red on day one and
# stay red, so it would be turned off — which is how a config ends up in the repository that nobody
# runs, the exact state this task found .clang-tidy in. The format gate made the same call for the
# same reason and the tree has been converging under it ever since: code you TOUCH becomes clean.
CHANGED=$(git diff --name-only --diff-filter=ACMR "$BASE" -- '*.cpp' '*.hpp' | sed "s#^#$ROOT/#")
if [ -z "$CHANGED" ]; then
    echo "clang-tidy: no C++ files changed vs $BASE — nothing to analyse"
    exit 0
fi
N_CHANGED=$(printf '%s\n' "$CHANGED" | grep -c .)

# The database holds translation units, so a changed HEADER is never in it and a changed .cpp that
# belongs to no project is not either. Counting the overlap is what separates "this change has
# nothing to analyse" from "the database is stale and I am about to pass on nothing".
COVERED=$(CHANGED_FILES="$CHANGED" python3 -c '
import json, os
db = {e["file"] for e in json.load(open("compile_commands.json"))}
print(sum(1 for l in os.environ["CHANGED_FILES"].split() if l in db))')
echo "changed C++ files vs $BASE: $N_CHANGED, of which $COVERED are translation units in the database"
if [ "$COVERED" -eq 0 ] && printf '%s\n' "$CHANGED" | grep -q '\.cpp$'; then
    echo "clang-tidy: $N_CHANGED C++ files changed, .cpp among them, yet NONE is in" >&2
    echo "compile_commands.json. The database does not describe this tree — regenerate the project" >&2
    echo "files ('CI=true premake5 gmake2'). Refusing to report a pass over zero files." >&2
    exit 2
fi

DIFFPY="$TIDY_DIR/../share/clang/clang-tidy-diff.py"
if [ ! -f "$DIFFPY" ]; then
    echo "clang-tidy: clang-tidy-diff.py not found at $DIFFPY (part of the llvm@18 keg)." >&2
    exit 2
fi

# -p1 because `git diff` prefixes a/ and b/. -use-color 0 keeps the log readable in Actions.
OUT=$(git diff -U0 "$BASE" -- '*.cpp' '*.hpp' \
      | python3 "$DIFFPY" -clang-tidy-binary "$TIDY" -p1 -path "$ROOT" -j "$JOBS" \
                -use-color 0 -quiet "${EXTRA[@]}" 2>&1)
RC=$?
printf '%s\n' "$OUT"
if [ "$RC" -eq 0 ]; then
    echo "clang-tidy: the lines you changed are clean (vs $BASE)"
    exit 0
fi
echo ""
echo "clang-tidy findings in the lines you changed (exit $RC). Reproduce locally with:"
echo "  scripts/CI/CheckTidy.sh $BASE"
exit 1
