#!/usr/bin/env bash
# tree_leaks.sh <Suite…> — run the named built suites from the tree root and fail if any of them left something
# new in the checkout; names the suite and the entry. handoff_check.sh (step h) sources this file for the same
# check over every suite, so the gate and this standalone probe are one code path.
#
# Why: on 09-26 the owner found DerivedDataCache/ (bucket files) and Assets/Library/ in the root of a Windows tree
# after a run: tests without a project wrote relative to the working directory, which is the tree root here.

# The tree as git sees it: untracked files one by one (a write into a directory that was ALREADY untracked, like an
# editor's leftover Meshes/Modeling/, is a new line too), ignored entries only as the path their .gitignore pattern
# names (--ignored=matching: build/ is one line, not four thousand, so a write into an ignored directory that
# already existed is not seen). A suite that writes into the checkout shows up here as a new line, whether
# .gitignore hides it or not. Git does not list EMPTY directories, and a scratch file removed at teardown leaves
# exactly that (RegistryProbe/Content/ sat in the root unseen), so the root's own entries are listed too.
tree_state() {
    { git status --porcelain --ignored=matching --untracked-files=all 2>/dev/null
      for e in * .[!.]*; do [ -e "$e" ] && printf '?? %s\n' "$e"; done; } | LC_ALL=C sort -u
}

# A unit is one manifest entry, `<runner binary>:<Suite>` (build/TestManifest.txt lists `<Executable> <Suite>`);
# suites live inside the layer runners (CommonTests, EngineTests, ...) and one runs as `<runner> --desert-suite=<Suite>`,
# exactly as suite.sh runs it. Logs, run windows and exit codes are keyed by the suite name.
unit_bin()   { printf '%s' "${1%:*}"; }
unit_suite() { printf '%s' "${1##*:}"; }

# run_one <unit> <logdir>: one suite from its own scratch directory, recording its wall-clock window (.t0/.t1) so a
# file that appears in the tree can be traced to the suites that were running when it was born. Capped at
# HANDOFF_SUITE_TIMEOUT seconds (900: on a loaded machine 300 s cut CloudType/RenderGraphCompile short of a verdict).
run_one() {
    local bin suite log scratch
    suite=$(unit_suite "$1")
    date +%s >"$2/$suite.t0"
    # Each suite runs in its own emptied scratch directory, exactly as CI's RunTests.sh does: from the tree root a
    # suite that reads tracked files relative to the working directory passed here and failed in CI (int/thm, 5 suites).
    bin="$(cd "$(dirname "$(unit_bin "$1")")" && pwd)/$(basename "$(unit_bin "$1")")"; log="$(cd "$2" && pwd)"
    scratch="$PWD/build/TestScratch/Debug/$suite"
    rm -rf "$scratch" && mkdir -p "$scratch"
    (cd "$scratch" && dev_capped "${HANDOFF_SUITE_TIMEOUT:-900}" "$bin" --desert-suite="$suite" </dev/null >"$log/$suite.log" 2>&1)
    echo $? >"$log/$suite.rc"
    date +%s >"$2/$suite.t1"
}

# manifest_unit <Suite>: the unit of a suite named in build/TestManifest.txt, or nothing.
manifest_unit() {
    awk -v s="$1" -v b="build/Bin/Tests/Debug" '$2 == s { print b "/" $1 ":" $2 }' build/TestManifest.txt 2>/dev/null
}

# trace_leaks <logdir> <unit…>: compares <logdir>/tree.before with the tree now. Each new entry is traced to the
# suites whose run window contains its birth; each candidate is then re-run ALONE with the entry moved aside, and
# the one that recreates it is named. Entries are moved to <logdir>/leaked (never deleted) so the tree is clean for
# the next run. Appends one line per finding to the caller's `red` array; returns 1 if anything leaked.
trace_leaks() {
    local log="$1"; shift
    local units=("$@") leaked entry born t n t0 t1 w
    tree_state >"$log/tree.after"
    leaked=$(LC_ALL=C comm -13 "$log/tree.before" "$log/tree.after" | sed 's/^.. //; s/^"//; s/"$//; s:/$::' | LC_ALL=C sort -u)
    [ -z "$leaked" ] && return 0
    mkdir -p "$log/leaked"
    while IFS= read -r entry; do
        [ -e "$entry" ] || continue
        born=$(stat -f %B "$entry" 2>/dev/null); [[ "$born" =~ ^[0-9]+$ ]] || born=$(stat -c %W "$entry" 2>/dev/null || echo 0)
        local candidates=() writers=()
        for t in "${units[@]}"; do
            n=$(unit_suite "$t"); t0=$(cat "$log/$n.t0" 2>/dev/null || echo 0); t1=$(cat "$log/$n.t1" 2>/dev/null || echo 0)
            [ "$born" -ge "$((t0 - 1))" ] && [ "$born" -le "$((t1 + 1))" ] && candidates+=("$t")
        done
        for t in "${candidates[@]:+${candidates[@]}}"; do
            [ -e "$entry" ] && mv "$entry" "$log/leaked/$(echo "$entry" | tr / _).$RANDOM$RANDOM"
            run_one "$t" "$log/leaked"
            [ -e "$entry" ] && writers+=("$(unit_suite "$t")")
        done
        [ -e "$entry" ] && mv "$entry" "$log/leaked/$(echo "$entry" | tr / _).$RANDOM$RANDOM"
        if [ ${#writers[@]} -gt 0 ]; then
            for w in "${writers[@]}"; do red+=("suite $w wrote $entry into the tree (moved to $log/leaked)"); done
        else
            red+=("$entry appeared in the tree during the suites; running then: $(for t in "${candidates[@]:+${candidates[@]}}"; do unit_suite "$t"; echo; done | tr '\n' ' ')(no single suite recreated it alone; moved to $log/leaked)")
        fi
    done <<<"$leaked"
    return 1
}

# Sourced by handoff_check.sh: only the functions above.
[ "${BASH_SOURCE[0]}" != "$0" ] && return 0

set -u
source "$(dirname "$0")/_common.sh"
[ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] || [ $# -eq 0 ] && { dev_help "$0"; exit 0; }
cd "$DEV_ROOT" || exit 2
LOG=$(dev_logdir tree-leaks)
units=()
for s in "$@"; do
    u=$(manifest_unit "$s")
    [ -n "$u" ] || { echo "tree_leaks: $s is not in build/TestManifest.txt"; exit 2; }
    [ -x "$(unit_bin "$u")" ] || { echo "tree_leaks: $(unit_bin "$u") is not built — suite.sh $s first"; exit 2; }
    units+=("$u")
done
red=()
tree_state >"$LOG/tree.before"
for u in "${units[@]}"; do run_one "$u" "$LOG"; done
trace_leaks "$LOG" "${units[@]}"
for u in "${units[@]}"; do
    n=$(unit_suite "$u")
    [ "$(cat "$LOG/$n.rc")" = 0 ] || red+=("suite $n failed (exit $(cat "$LOG/$n.rc"), $LOG/$n.log)")
done
if [ ${#red[@]} -gt 0 ]; then printf 'RED: %s\n' "${red[@]}"; echo "log: $LOG"; exit 1; fi
echo "clean: ${#units[@]} suite(s) left the tree as they found it (log: $LOG)"
