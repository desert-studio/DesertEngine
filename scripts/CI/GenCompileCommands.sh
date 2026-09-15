#!/usr/bin/env bash
# Emit compile_commands.json for the WHOLE workspace, from the makefiles premake already generated.
#
# WHY THIS EXISTS AND WHY IT IS NOT `bear`/`compiledb`:
#   clang-tidy cannot parse a translation unit without the exact flags it is compiled with, and
#   premake's gmake2 action does not write a compilation database. The three usual answers were
#   weighed and two were rejected:
#     * `bear` interposes on the linker/loader to watch a REAL build, so it costs a full build every
#       time the database is refreshed (~20 min here) and, on macOS, its interposition is the thing
#       SIP exists to stop. A database you can only get by building is a database that goes stale.
#     * a premake export-compile-commands module is a NEW THIRD-PARTY DEPENDENCY vendored into the
#       build system, which the contract does not allow without agreement, and it re-derives the
#       flags from premake's own tables rather than from what make will actually run.
#   `make -n` is neither: it asks the build system itself, in dry-run, to print the commands it WOULD
#   run. The flags are therefore the build's flags by construction — not a second opinion about them
#   that can drift. It costs about two seconds and needs nothing compiled.
#
# THE TWO NON-OBVIOUS ARGUMENTS TO make, both of which are load-bearing:
#   verbose=1  premake's makefiles set SILENT=@ unless `verbose` is defined, so without it a dry run
#              prints the `echo Foo.cpp` progress lines and NOT the compiler command.
#   LDDEPS=    a link line depends on the static libraries of other projects, and in a dry run those
#              files do not exist and this makefile has no rule for them, so make aborts with
#              "No rule to make target `build/Bin/Debug/libDesert.a'". It aborts AFTER printing most
#              of the compile commands, which is the dangerous part: the run looks almost complete.
#              Measured: Editor.make printed 104 of its 105 compile commands and exited 2. Clearing
#              LDDEPS from the command line (which overrides the makefile's `+=`) removes the
#              unbuildable prerequisites, and every makefile then exits 0 with a complete list.
#
# ThirdParty is excluded BY PATH COMPONENT, not by a list of names: any source whose path contains a
# `ThirdParty/` component is dropped. A new submodule lands under ThirdParty/ and is excluded the day
# it arrives, with nothing to remember to edit.
#
# Usage: scripts/CI/GenCompileCommands.sh [Debug|Release] [-o <output>]
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD"

CONFIG="Debug"
OUT="$ROOT/compile_commands.json"
while [ $# -gt 0 ]; do
    case "$1" in
        Debug|Release) CONFIG="$1"; shift ;;
        -o) OUT="$2"; shift 2 ;;
        *) echo "GenCompileCommands: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
MAKE_CONFIG="$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"

# EVERY FAILURE BELOW EXITS 2 AND NAMES ITSELF. An empty or partial database is the exact shape this
# project keeps paying for — a tool that answers a different question with no sign that it did — and
# downstream the answer would be "0 files analysed, no warnings", i.e. a silent pass.
if [ ! -f "$ROOT/Makefile" ]; then
    echo "GenCompileCommands: no Makefile in $ROOT — run 'CI=true premake5 gmake2' first." >&2
    exit 2
fi
# THE LIST COMES FROM PREMAKE, NOT FROM A GLOB, and the difference is an exit 2 rather than a warning.
# `premake5 gmake` writes makefiles and never deletes one whose project is gone, so a renamed or removed
# suite leaves an orphan behind in every tree and every worktree. `make -n -f <orphan>.make` fails, and
# because this script feeds every makefile to `make -n`, ONE orphan kills the whole compiler database —
# which surfaces downstream as "clang-tidy: the gate could not run", a failure with nothing in it that
# points here. Measured 2026-09-15: Ю16 renamed PreviewSlotBudget to RendererSlotBudget and the next
# CheckTidy.sh in a worktree returned 2 for that reason alone. (The sweep in the verify skill had the
# same glob and the same defect; it was corrected the same day, where one orphan cost one BUILD-FAIL
# instead of the whole run.)
#
# The root `Makefile` carries premake's own `PROJECTS :=` line and cannot name a project premake does
# not know, so it is the honest source. Do NOT try to spot orphans by timestamp: premake rewrites only
# the files that changed, so almost every makefile is older than the newest one.
PROJECTS_LINE=$(sed -n 's/^PROJECTS := //p' "$ROOT/Makefile")
if [ -z "$PROJECTS_LINE" ]; then
    echo "GenCompileCommands: $ROOT/Makefile has no 'PROJECTS :=' line — regenerate with premake." >&2
    exit 2
fi
MAKEFILES=()
for proj in $PROJECTS_LINE; do
    [ -f "$ROOT/$proj.make" ] && MAKEFILES+=( "$ROOT/$proj.make" )
done
if [ ${#MAKEFILES[@]} -eq 0 ]; then
    echo "GenCompileCommands: no *.make in $ROOT — run 'CI=true premake5 gmake2' first." >&2
    exit 2
fi

TMP="$(mktemp -d)"
# The partial file gets a UNIQUE name and not "$OUT.partial". Two gates can run in one tree at the
# same moment — scripts/MacOS/BuildMacOS.sh ends by invoking CheckTidy.sh, and a developer running
# CheckTidy.sh by hand alongside it is ordinary — and with a fixed sibling name the first `mv` takes
# the file the second is still writing, so the second dies on "No such file or directory" and the gate
# reports an environment failure that is really a collision. Measured here on 2026-09-14.
PARTIAL="$(mktemp "${OUT}.partial.XXXXXX" 2>/dev/null || echo "$TMP/compile_commands.json")"
trap 'rm -rf "$TMP"; rm -f "$PARTIAL"' EXIT

TOTAL=0
KEPT=0
MISMATCH=()

{
    echo "["
    FIRST=1
    for mk in "${MAKEFILES[@]}"; do
        PROJ="$(basename "$mk" .make)"
        set +e
        make -n -B -f "$mk" config="$MAKE_CONFIG" verbose=1 LDDEPS= >"$TMP/out" 2>"$TMP/err"
        RC=$?
        set -e
        if [ $RC -ne 0 ]; then
            echo "GenCompileCommands: 'make -n' FAILED for $PROJ (exit $RC)" >&2
            sed 's/^/    /' "$TMP/err" >&2
            exit 2
        fi

        # WHAT THIS PROJECT CLAIMS TO COMPILE, read from the makefile itself. Checking the dry run
        # against it is the point: a truncated `make -n` still prints most of its commands, so a
        # count against zero would not notice. premake emits exactly one `OBJECTS += $(OBJDIR)/x.o`
        # per source, shared by both configurations. Aggregate projects (BuildAllTests, RunAllTests)
        # declare none, which is why the expectation is DERIVED from the file rather than being a
        # list of project names somebody has to keep up to date.
        DECLARED=$(grep -c '^OBJECTS += ' "$mk" || true)
        N=0
        # A compile line starts with the compiler and carries `-c "<source>"`. The PCH build is
        # skipped: `-x c++-header` compiling pch.hpp is not a translation unit anyone lints, and the
        # object it produces is a .gch.
        while IFS= read -r line; do
            case "$line" in
                clang\ *|clang++\ *) ;;
                *) continue ;;
            esac
            case "$line" in *" -x c++-header "*) continue ;; esac
            # The LINK line also begins with `clang++` and must not be mistaken for a compile. It is
            # told apart by `-c "`, not by position: an earlier version of this loop tested whether
            # the substring extraction had changed the string, which is true for a link line too, so
            # every executable project came out one entry over its own source list.
            case "$line" in *' -c "'*) ;; *) continue ;; esac
            SRC="${line##*-c \"}"
            SRC="${SRC%\"*}"
            [ -n "$SRC" ] || continue
            TOTAL=$((TOTAL + 1))
            N=$((N + 1))
            case "/$SRC" in */ThirdParty/*) continue ;; esac

            # Drop the precompiled header. premake emits BOTH `-include <objdir>/pch.hpp` (which
            # resolves to the .gch beside it) and a plain `-include pch.hpp`, so removing the first
            # loses nothing: the header is still included, from source, through -I. Keeping it would
            # hand clang-tidy 18 a .gch produced by whatever clang the build used — a hard error when
            # the two versions differ, and a stale answer when they do not.
            # The source is rewritten to an ABSOLUTE path. libTooling matches the entry's `file`
            # against the argument inside the command; with a repo-relative argument and an absolute
            # `file` the two never meet, and clang-tidy answers "no input files" — an error, but one
            # that a per-file loop could easily count as "this file produced no warnings".
            CMD="$(printf '%s' "$line" | sed -E \
                -e 's#-include build/[^ ]*/pch\.hpp ##g' \
                -e 's#-MD ##g; s#-MP ##g' \
                -e 's#-MF "[^"]*" ##g' \
                -e 's#-o "[^"]*" ##g' \
                -e "s#-c \"${SRC}\"#-c \"${ROOT}/${SRC}\"#")"

            ESC="$(printf '%s' "$CMD" | sed -e 's#\\#\\\\#g' -e 's#"#\\"#g')"
            [ $FIRST -eq 1 ] || echo ","
            FIRST=0
            printf '{"directory": "%s", "file": "%s", "command": "%s"}' \
                "$ROOT" "$ROOT/$SRC" "$ESC"
            KEPT=$((KEPT + 1))
        done <"$TMP/out"

        [ "$N" -eq "$DECLARED" ] || MISMATCH+=("$PROJ($N/$DECLARED)")
    done
    echo ""
    echo "]"
} >"$PARTIAL"

if [ ${#MISMATCH[@]} -gt 0 ]; then
    echo "GenCompileCommands: dry run disagrees with the makefile's own source list" >&2
    echo "  (seen/declared): ${MISMATCH[*]}" >&2
    exit 2
fi
if [ "$KEPT" -eq 0 ]; then
    echo "GenCompileCommands: produced an EMPTY database from ${#MAKEFILES[@]} makefiles." >&2
    exit 2
fi

mv "$PARTIAL" "$OUT"
echo "compile_commands.json: $KEPT entries from ${#MAKEFILES[@]} projects ($CONFIG); \
$((TOTAL - KEPT)) ThirdParty entries excluded -> $OUT"
