#!/usr/bin/env bash
# handoff_check.sh [base=origin/dev] — everything a branch must pass before hand-off, in one call.
# Runs every built test in build/Bin/Tests/Debug from the tree root (exit codes, 300 s cap, HANDOFF_JOBS=4 in
# parallel, reds re-run alone), RepoOnlyIncludes --require-replacements, llvm@18 clang-format on changed lines,
# and the .claude guard; warns on test binaries older than their .o or linked libs. Exit 0 = all green.
# On green with a clean tree, writes .cache/handoff/<full HEAD sha>.ok (the summary line); any red deletes it.
set -u
source "$(dirname "$0")/_common.sh"
[ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] && { dev_help "$0"; exit 0; }
BASE="${1:-origin/dev}"
cd "$DEV_ROOT" || exit 2
git rev-parse -q --verify "$BASE^{commit}" >/dev/null || { echo "handoff_check: unknown base '$BASE'"; exit 2; }
HEAD_SHA=$(git rev-parse HEAD)
MARKER="$DEV_ROOT/.cache/handoff/$HEAD_SHA.ok"
rm -f "$MARKER"
# Keep the last five run directories; the older ones are only disk.
ls -dt build/DevLogs/handoff-* 2>/dev/null | tail -n +6 | xargs rm -rf
LOG=$(dev_logdir handoff)
start=$(date +%s)
fail=0

# (a) every test binary, from the root: several suites resolve fixtures relative to the working directory.
# Parallel for time; a suite that fails in parallel is re-run ALONE before it counts as red, so two suites
# sharing a temp path cannot manufacture a red.
BIN=build/Bin/Tests/Debug
run_one() { dev_capped 300 "$1" </dev/null >"$2/$(basename "$1").log" 2>&1; echo $? >"$2/$(basename "$1").rc"; }
export -f run_one dev_capped
dev_regen_makefiles "$LOG" || exit 2
# A suite deleted on this branch keeps its old .make (premake never removes one), and the build below stops on
# "No rule to make target" — LODFold did this in five trees on 09-26. A test makefile that the regeneration did not
# rewrite names a suite that no longer exists: park it (never delete) so the list below is what premake made.
for mk in $(grep -l "TARGETDIR = build/Bin/Tests/Debug" ./*.make 2>/dev/null); do
    [ "$mk" -ot Makefile ] && { mkdir -p build/stale-make; mv "$mk" build/stale-make/; echo "handoff_check: parked stale $mk"; }
done
# (0) Build every suite first, in one make: after a merge 200+ binaries predate libCommon.a and a new suite has no
# binary at all, and an agent that fixed that by hand spent a whole second pass (~45 min) per branch, five times on
# 09-25. make rebuilds only what is out of date, so on a fresh tree this is seconds. HANDOFF_NO_BUILD=1 skips it.
if [ -z "${HANDOFF_NO_BUILD:-}" ]; then
    suites=$(grep -l "TARGETDIR = build/Bin/Tests/Debug" ./*.make 2>/dev/null | xargs -n1 basename | sed 's/\.make$//')
    if ! printf '%s\n' $suites | xargs "$DEV_ROOT/scripts/Dev/suite.sh" --build-only >"$LOG/build.log" 2>&1; then
        echo "handoff_check: building the suites FAILED; log $LOG/build.log"; tail -5 "$LOG/build.log"; exit 1
    fi
    # (0b) the Editor too: suites compile a fraction of the Editor's sources, and on 09-26 two batches each passed
    # every suite while together they broke the Editor build (AV1d changed PreviewViewport::Draw, AV1e called the
    # old one). HANDOFF_NO_EDITOR=1 skips it.
    if [ -z "${HANDOFF_NO_EDITOR:-}" ] && [ -f Editor.make ]; then
        if ! "$HOME/.claude/tools/build_quiet.sh" "$PWD" "$LOG/editor.log" Editor >/dev/null 2>&1; then
            echo "handoff_check: building the Editor FAILED; log $LOG/editor.log"; grep -m5 "error:" "$LOG/editor.log"; exit 1
        fi
    fi
fi
bins=()
for t in "$BIN"/*; do [ -f "$t" ] && [ -x "$t" ] && bins+=("$t"); done
total=${#bins[@]}
[ "$total" -gt 0 ] && printf '%s\n' "${bins[@]}" | xargs -P "${HANDOFF_JOBS:-4}" -I{} bash -c 'run_one "$1" "$2"' _ {} "$LOG"
passed=0; red=(); stale=()
# (f) never built: a suite whose makefile exists but whose binary does not is not "not run", it is untested code.
# The AF7 branch passed 146/146 in a tree that had built 146 of 332 suites; ModelingToolTarget no longer compiled.
for mk in $(grep -l "TARGETDIR = build/Bin/Tests/Debug" ./*.make 2>/dev/null); do
    name=$(basename "$mk" .make)
    [ -x "$BIN/$name" ] || stale+=("$name<never built")
done
for t in "${bins[@]}"; do
    name=$(basename "$t")
    rc=$(cat "$LOG/$name.rc" 2>/dev/null || echo 255)
    if [ "$rc" != 0 ]; then run_one "$t" "$LOG"; rc=$(cat "$LOG/$name.rc"); fi
    if [ "$rc" = 0 ]; then passed=$((passed + 1)); else
        tests=$(grep -a '^\[  FAILED  \] [A-Za-z_]' "$LOG/$name.log" | grep -av 'listed below' |
                sed 's/^\[  FAILED  \] //; s/ (.*//; s/,.*//' | sort -u | head -3 | tr '\n' ' ')
        [ "$rc" = 124 ] && tests="TIMEOUT 300s"
        red+=("$name rc=$rc $tests")
    fi
    # (e) stale: the binary must be newer than its own objects and every library it links (Debug LDDEPS).
    newest=$(ls -t "build/Tests/Intermediates/Debug/Debug/$name"/*.o 2>/dev/null | head -1)
    if [ -n "$newest" ] && [ "$newest" -nt "$t" ]; then stale+=("$name<$(basename "$newest")"); continue; fi
    # BuildVersion compares its baked commit count/hash with git's answer, so every commit after its
    # build turns it red; say "stale" rather than let an agent debug a correct test.
    if [ "$name" = BuildVersion ] && [ "$(stat -f %m "$t")" -lt "$(git log -1 --format=%ct HEAD)" ]; then
        stale+=("$name<HEAD commit"); continue
    fi
    [ -f "$name.make" ] || continue
    for lib in $(awk '/^ifeq \(\$\(config\),debug\)/{d=1} /^ifeq \(\$\(config\),release\)/{d=0}
                      d && /^LDDEPS \+=/{for(i=3;i<=NF;i++)print $i}' "$name.make"); do
        if [ -f "$lib" ] && [ "$lib" -nt "$t" ]; then stale+=("$name<$(basename "$lib")"); break; fi
    done
done
[ ${#red[@]} -gt 0 ] && fail=1
[ "$total" -eq 0 ] && { red+=("no test binaries in $BIN — build the suites first"); fail=1; }

# (b) repo-only includes
if bash scripts/CI/RepoOnlyIncludes.sh --require-replacements >"$LOG/includes.log" 2>&1; then inc=ok; else inc=RED; fail=1; fi

# (c) clang-format 18 on the changed lines (Homebrew v22 disagrees with CI; the gate names its binary)
if PATH="/opt/homebrew/opt/llvm@18/bin:$PATH" bash scripts/CI/CheckFormat.sh "$BASE" >"$LOG/format.log" 2>&1; then fmt=ok; else fmt=RED; fail=1; fi
# (g) clang-tidy on the changed lines, as CI runs it: the 09-24 batch reached dev with 13 tidy errors because no
# gate before the merge ran it (agents never run tidy on their own — the one pass is here, once per hand-off).
if PATH="/opt/homebrew/opt/llvm@18/bin:$PATH" bash scripts/CI/CheckTidy.sh "$(git merge-base "$BASE" HEAD)" >"$LOG/tidy.log" 2>&1; then tidy=ok; else tidy=RED; fail=1; fi

# (d) .claude: an agent cannot commit it, so anything the branch would bring into base on merge is a stale
# or foreign copy (one such merge nearly rolled back the guard). Measured from the merge-base: a branch that
# is merely behind base is not red, but a merge of base that kept an old .claude IS.
mb=$(git merge-base "$BASE" HEAD)
git diff --stat "$mb" HEAD -- .claude >"$LOG/claude.log" 2>&1
if [ -s "$LOG/claude.log" ]; then cl=RED; fail=1; else cl=ok; fi
behind=""; git diff --quiet "$BASE" HEAD -- .claude || behind=" (.claude differs from base itself)"

# The verdict is about a COMMIT; with tracked edits outstanding it is about a working tree nobody can name.
dirty=""; git diff --quiet HEAD -- . ':!.cache' || dirty=" DIRTY tree: no marker"

secs=$(( $(date +%s) - start ))
line="$passed/$total suites, includes $inc, format $fmt, tidy $tidy, .claude $cl — ${secs}s"
{ echo "$line"; printf 'RED %s\n' "${red[@]:+${red[@]}}"; printf 'STALE %s\n' "${stale[@]:+${stale[@]}}"; } >"$LOG/summary.txt"
echo "$line$behind$dirty; logs $LOG"
i=0; for r in "${red[@]:+${red[@]}}"; do i=$((i + 1)); [ $i -le 6 ] && echo "  RED $r"; done
[ ${#red[@]} -gt 6 ] && echo "  ... $(( ${#red[@]} - 6 )) more in $LOG/summary.txt"
[ "$cl" = RED ] && echo "  .claude changed vs merge-base: $(grep -c '|' "$LOG/claude.log") file(s) — lead must fix"
# A stale binary tests old code: T6c4 got "145/146 green" with 119 of them stale and a suite that no longer compiled.
# So stale is a failure, not a warning — the marker must mean "this commit's code passed".
if [ ${#stale[@]} -gt 0 ]; then
    fail=1
    echo "  STALE ${#stale[@]} test binary(ies), e.g. ${stale[0]} — no marker. Rebuild: scripts/Dev/suite.sh $(printf '%s\n' "${stale[@]}" | sed 's/<.*//' | sort -u | tr '\n' ' ')" | cut -c1-600
fi
if [ $fail -eq 0 ] && [ -z "$dirty" ]; then
    mkdir -p "$(dirname "$MARKER")" && echo "$line" >"$MARKER" && echo "  marker $MARKER"
fi
exit $fail
