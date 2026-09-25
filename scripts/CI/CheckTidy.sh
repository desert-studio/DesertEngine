#!/usr/bin/env bash
# clang-tidy gate. Two modes:
#   scripts/CI/CheckTidy.sh [<base-ref-or-sha>]   changed lines only, against the merge-base,
#                                                 AND the register below
#   scripts/CI/CheckTidy.sh --all                 every translation unit in the workspace
#   scripts/CI/CheckTidy.sh --register            only scripts/CI/TidyRegister.txt
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

# ONE TIDY AT A TIME ON A DEVELOPER MACHINE, AND AT MOST FOUR JOBS. On 2026-09-25 23:11 several agents
# ran this gate at once, each with -j10: 21 clang-tidy processes held 6.4 GB beside 18 clang and an
# editor, the 16 GB machine ran out, and WindowServer's watchdog killed the session twice. The build
# hook already serialises `make`; tidy was the uncounted twin. CI runners are single-tenant and keep
# the full CPU count.
if [ -z "${CI:-}" ]; then
    [ "$JOBS" -gt 4 ] && JOBS=4
    TIDY_LOCK="${TMPDIR:-/tmp}/desert-clang-tidy.lock"
    waited=0
    until mkdir "$TIDY_LOCK" 2>/dev/null; do
        holder=$(cat "$TIDY_LOCK/pid" 2>/dev/null)
        if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then
            rm -rf "$TIDY_LOCK"   # the holder died without releasing it
            continue
        fi
        [ $((waited % 60)) -eq 0 ] && echo "clang-tidy: another CheckTidy.sh (pid ${holder:-?}) is running; waiting (${waited}s)" >&2
        sleep 10
        waited=$((waited + 10))
    done
    echo $$ > "$TIDY_LOCK/pid"
    trap 'rm -rf "$TIDY_LOCK"' EXIT
fi

# THE EXIT CODE IS THE VERDICT, AND IT COMES FROM .clang-tidy's `WarningsAsErrors: '*'`.
# clang-tidy exits 0 with findings unless warnings are errors, and run-clang-tidy and
# clang-tidy-diff.py both report the maximum child exit code — so without that setting this gate
# would be green over a wall of diagnostics. Nothing here re-asserts it: one place decides, and a
# developer running clang-tidy by hand gets the same verdict CI does.


# ------------------------------------------------------------------------------------------------------
# THE REGISTER: files that were cleaned of a defect CLASS and must stay clean for it, in every line and
# not only in the ones somebody is touching. scripts/CI/TidyRegister.txt carries the rows and the argument
# for each check; this is the part that runs them.
#
# It exists because the changed-lines gate has one blind spot by construction: a file nobody edits cannot
# regress into it. Forty-three unchecked optional dereferences, three memcmps over float aggregates and
# fourteen truncating roundings were all sitting in files that had passed every changed-lines run there
# has ever been.
RunRegister() {
    local register="$ROOT/scripts/CI/TidyRegister.txt"
    if [ ! -f "$register" ]; then
        echo "clang-tidy: $register is missing; the register gate cannot run." >&2
        return 2
    fi

    local checks files
    checks=$(awk '/^CHECKS:/{c=1;next} /^FILES:/{c=0} c && /^[a-z]/ {printf "%s%s", sep, $0; sep=","}' "$register")
    files=$(awk '/^FILES:/{f=1;next} f && /^[A-Za-z]/ {print}' "$register")

    # A REGISTER THAT CAME OUT EMPTY IS AN ENVIRONMENT FAILURE, NOT A PASS. This is the same shape the
    # exit codes at the top of this file exist for: nothing to analyse reads exactly like nothing wrong.
    if [ -z "$checks" ] || [ -z "$files" ]; then
        echo "clang-tidy: the register parsed to $(printf '%s' "$checks" | wc -c) bytes of checks and" >&2
        echo "  $(printf '%s\n' "$files" | grep -c .) file(s). One of its two sections is missing or" >&2
        echo "  its format changed. This is an environment failure, NOT a clean register." >&2
        return 2
    fi

    local missing=""
    local absolute=""
    local row
    while IFS= read -r row; do
        [ -n "$row" ] || continue
        if [ ! -f "$ROOT/$row" ]; then
            missing="$missing $row"
            continue
        fi
        absolute="$absolute$ROOT/$row"$'\n'
    done <<< "$files"

    if [ -n "$missing" ]; then
        echo "clang-tidy: the register names files that do not exist:" >&2
        printf '    %s\n' $missing >&2
        echo "A row is a claim about a file. Renaming one means moving its row, not dropping it." >&2
        return 2
    fi

    local rows
    rows=$(printf '%s' "$absolute" | grep -c .)
    echo "--- register: $rows file(s) x $(printf '%s' "$checks" | tr ',' '\n' | grep -c .) check(s), -j$JOBS"

    local log
    log=$(mktemp)
    printf '%s' "$absolute" | tr '\n' '\0' \
        | xargs -0 -P "$JOBS" -n 1 -I{} "$TIDY" -p "$ROOT" -quiet -checks="-*,$checks" \
                ${EXTRA[@]+"${EXTRA[@]}"} {} > "$log" 2>&1
    local rc=$?

    # THE EXIT CODE OF xargs IS NOT ENOUGH: it reports 123 for "some child failed" and 0 otherwise, and a
    # clang-tidy that could not parse a file exits non-zero for a reason that is not a finding. The
    # diagnostics themselves are what is read, and their absence is checked against the row count above.
    if grep -qE ': (error|warning): ' "$log"; then
        echo ""
        grep -E ': (error|warning): ' "$log" | sed "s#$ROOT/##" | sort -u
        echo ""
        echo "clang-tidy: a file in scripts/CI/TidyRegister.txt shows a defect class it was cleaned of."
        echo "Reproduce with: scripts/CI/CheckTidy.sh --register"
        rm -f "$log"
        return 1
    fi

    if [ "$rc" -ne 0 ]; then
        echo "clang-tidy: the register run exited $rc with no diagnostics — the analyser failed to run" >&2
        echo "  over at least one row. This is an environment failure, NOT a clean register." >&2
        sed -n '1,20p' "$log" >&2
        rm -f "$log"
        return 2
    fi

    rm -f "$log"
    echo "clang-tidy: the register's $rows file(s) are clean"
    return 0
}

if [ "${1:-}" = "--register" ]; then
    RunRegister
    exit $?
fi

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

# A THIRD CAUSE OF ABSENCE, AND THE MESSAGE BELOW USED TO BLAME THE WRONG TWO.
#
# `Desert/Common/premake5.lua:99` carries `filter { "system:not windows" } removefiles
# { "Source/Common/Platform/Windows/**" }`, and this gate runs on macOS by deliberate design. So a
# Windows-only source can NEVER appear in this database: it is not "premake was not re-run" and not
# "missing from a premake5.lua" — this platform does not compile that file, by construction, and no
# amount of re-running changes it.
#
# The gate already separates "I found violations" from "I could not run". This is the distinction one
# level finer — "I could not run HERE" — and it is the same split RepoOnlyIncludes.sh needed on the
# same day, for its submodule half. "Cannot answer here" and "answered no" must not share an exit
# code, or a platform's files become unreviewable the moment anyone touches them.
#
# NAMED, NOT SILENT. A silent skip is how coverage disappears: this project spent a day undoing a
# gate that skipped Windows and macOS entirely while looking like an ordinary red. These files are
# listed on every run, so the hole is visible in the log rather than inferred from its absence.
PLATFORM_FOREIGN=""
TRUE_ORPHANS=""
for f in $ORPHANS; do
    case "$f" in
        # Any directory named Windows/Linux, not only Platform/: Editor/premake5.lua removes
        # Source/Editor/Splash/Windows/** off Windows, and the narrower pattern called the splash an
        # orphan — exit 2 on every push that touched it (SP1), reported as an environment failure.
        */Windows/*|*/Linux/*) PLATFORM_FOREIGN="$PLATFORM_FOREIGN $f" ;;
        *)                     TRUE_ORPHANS="$TRUE_ORPHANS $f" ;;
    esac
done

if [ -n "$PLATFORM_FOREIGN" ]; then
    echo "clang-tidy: NOT ANALYSABLE ON THIS RUNNER — these sources belong to a platform this host"
    echo "does not compile, so premake removed them from the build and they cannot be in the database:"
    printf '    %s\n' $PLATFORM_FOREIGN
    echo "They are skipped, not passed. Re-running premake will not change this; only running the gate"
    echo "on that platform would. This is a GAP IN COVERAGE, stated rather than hidden."
fi

if [ -n "$TRUE_ORPHANS" ]; then
    echo "clang-tidy: these changed sources are in NO premake project, so nothing compiles them and" >&2
    echo "the gate cannot know their flags:" >&2
    printf '    %s\n' $TRUE_ORPHANS >&2
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
# The skipped files are excluded from the DIFF as well, not only from the count: clang-tidy-diff.py
# would otherwise interpolate another project's flags for them, which is the trap documented above --
# it yields clang-diagnostic-errors that WarningsAsErrors reports as findings about your code.
EXCLUDE_PATHSPEC=""
for f in $PLATFORM_FOREIGN; do
    EXCLUDE_PATHSPEC="$EXCLUDE_PATHSPEC :(exclude)${f#$ROOT/}"
done

# A CHANGED HEADER IS ANALYSED WITH THE FLAGS OF A UNIT THAT INCLUDES IT, not whatever interpolation
# finds nearest. The interpolation measured right for CookedJsonWrite.hpp (above) is a similarity guess
# over file names, and in a directory whose sources are ALSO compiled by engine-free suites it guesses
# one of those: Editor/Core/DocumentWell.hpp borrowed AssetReferences.cpp's command from an engine-free
# test project, whose include paths lack Desert/Desert/Source, and IPanel.hpp's
# `#include <Engine/Assets/Common.hpp>` came back as a clang-diagnostic-error — a "finding" about a
# path the real build resolves. So each changed header gets its own entry, copied from a database unit
# whose source #includes it and resolves that include to this very file through its own -I list or its
# own directory; a production unit is preferred over a test one. A header nothing includes directly
# keeps the interpolation. The augmented database lives in a scratch directory and the checked-in
# workflow keeps reading compile_commands.json unchanged.
HDRDB=$(mktemp -d "${TMPDIR:-/tmp}/tidy-hdrdb.XXXXXX")
trap 'rm -rf "$HDRDB"; [ -n "${TIDY_LOCK:-}" ] && rm -rf "$TIDY_LOCK"' EXIT
if ! CHANGED_FILES="$CHANGED" OUT_DB="$HDRDB/compile_commands.json" python3 -c '
import json, os, re, shlex
db = json.load(open("compile_commands.json"))
names = {e["file"] for e in db}
headers = [f for f in os.environ["CHANGED_FILES"].split()
           if f.endswith((".hpp", ".h")) and f not in names and os.path.isfile(f)]
extra = []
if headers:
    inc = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", re.M)
    base = {os.path.basename(h) for h in headers}
    texts = {}
    for f in names:
        try:
            t = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        spelled = [s for s in inc.findall(t) if os.path.basename(s) in base]
        if spelled:
            texts[f] = spelled
    for h in headers:
        best = None
        for e in db:
            spelled = texts.get(e["file"])
            if not spelled:
                continue
            args = shlex.split(e["command"])
            dirs = [os.path.dirname(e["file"])]
            dirs += [a[2:] for a in args if a.startswith("-I") and len(a) > 2]
            dirs += [args[i + 1] for i, a in enumerate(args[:-1]) if a in ("-I", "-isystem", "-iquote")]
            full = [os.path.normpath(os.path.join(e["directory"], d, s)) for d in dirs for s in spelled]
            if h not in full:
                continue
            rank = 1 if "-DGTEST" in args else 0
            if best is None or rank < best[0]:
                best = (rank, e)
        if best:
            e = best[1]
            extra.append({"directory": e["directory"], "file": h,
                          "command": e["command"].replace(e["file"], h)})
json.dump(db + extra, open(os.environ["OUT_DB"], "w"))
print(f"headers given the flags of a unit that includes them: {len(extra)} of {len(headers)}")
'; then
    echo "clang-tidy: could not derive header commands from compile_commands.json. Gate did not run." >&2
    exit 2
fi

OUT=$(git diff -U0 "$BASE" -- '*.cpp' '*.hpp' '*.mm' '*.h' $EXCLUDE_PATHSPEC \
      | python3 -W ignore "$DIFFPY" -clang-tidy-binary "$TIDY" -p1 -path "$HDRDB" -j "$JOBS" \
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
        # AND THE REGISTER, in the same invocation, so CI picks it up without a second workflow step and
        # a developer cannot be clean on their diff while having reopened a class somewhere else.
        RunRegister
        exit $?
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
