#!/usr/bin/env bash
# tree_leaks.sh <Suite…> — run the named built suites from the tree root and fail if any of them left something
# new in the checkout; names the suite and the entry. handoff_check.sh (step h) sources this file for the same
# check over every suite, so the gate and this standalone probe are one code path.
#
# Why: on 09-26 the owner found DerivedDataCache/ (bucket files) and Assets/Library/ in the root of a Windows tree
# after a run: tests without a project wrote relative to the working directory, which is the tree root here.

# The tree as git sees it, ignored entries included (collapsed to their top directory): a suite that writes into
# the checkout shows up here as a new line, whether .gitignore hides it or not. Git does not list EMPTY
# directories, and a scratch file removed at teardown leaves exactly that (RegistryProbe/Content/ sat in the root
# unseen), so the root's own entries are listed too.
tree_state() {
    { git status --porcelain --ignored --untracked-files=normal 2>/dev/null
      for e in * .[!.]*; do [ -e "$e" ] && printf '?? %s\n' "$e"; done; } | LC_ALL=C sort -u
}

# run_one <binary> <logdir>: one suite from the tree root, recording its wall-clock window (.t0/.t1) so a file that
# appears in the tree can be traced to the suites that were running when it was born.
run_one() {
    date +%s >"$2/$(basename "$1").t0"
    dev_capped 300 "$1" </dev/null >"$2/$(basename "$1").log" 2>&1; echo $? >"$2/$(basename "$1").rc"
    date +%s >"$2/$(basename "$1").t1"
}

# trace_leaks <logdir> <binary…>: compares <logdir>/tree.before with the tree now. Each new entry is traced to the
# suites whose run window contains its birth; each candidate is then re-run ALONE with the entry moved aside, and
# the one that recreates it is named. Entries are moved to <logdir>/leaked (never deleted) so the tree is clean for
# the next run. Appends one line per finding to the caller's `red` array; returns 1 if anything leaked.
trace_leaks() {
    local log="$1"; shift
    local bins=("$@") leaked entry born t n t0 t1 w
    tree_state >"$log/tree.after"
    leaked=$(LC_ALL=C comm -13 "$log/tree.before" "$log/tree.after" | sed 's/^.. //; s/^"//; s/"$//; s:/$::' | LC_ALL=C sort -u)
    [ -z "$leaked" ] && return 0
    mkdir -p "$log/leaked"
    while IFS= read -r entry; do
        [ -e "$entry" ] || continue
        born=$(stat -f %B "$entry" 2>/dev/null); [[ "$born" =~ ^[0-9]+$ ]] || born=$(stat -c %W "$entry" 2>/dev/null || echo 0)
        local candidates=() writers=()
        for t in "${bins[@]}"; do
            n=$(basename "$t"); t0=$(cat "$log/$n.t0" 2>/dev/null || echo 0); t1=$(cat "$log/$n.t1" 2>/dev/null || echo 0)
            [ "$born" -ge "$((t0 - 1))" ] && [ "$born" -le "$((t1 + 1))" ] && candidates+=("$t")
        done
        for t in "${candidates[@]:+${candidates[@]}}"; do
            [ -e "$entry" ] && mv "$entry" "$log/leaked/$(echo "$entry" | tr / _).$RANDOM$RANDOM"
            run_one "$t" "$log/leaked"
            [ -e "$entry" ] && writers+=("$(basename "$t")")
        done
        [ -e "$entry" ] && mv "$entry" "$log/leaked/$(echo "$entry" | tr / _).$RANDOM$RANDOM"
        if [ ${#writers[@]} -gt 0 ]; then
            for w in "${writers[@]}"; do red+=("suite $w wrote $entry into the tree (moved to $log/leaked)"); done
        else
            red+=("$entry appeared in the tree during the suites; running then: $(for t in "${candidates[@]:+${candidates[@]}}"; do basename "$t"; done | tr '\n' ' ')(no single suite recreated it alone; moved to $log/leaked)")
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
bins=()
for s in "$@"; do
    t="build/Bin/Tests/Debug/$s"
    [ -x "$t" ] || { echo "tree_leaks: $t is not built — suite.sh $s first"; exit 2; }
    bins+=("$t")
done
red=()
tree_state >"$LOG/tree.before"
for t in "${bins[@]}"; do run_one "$t" "$LOG"; done
trace_leaks "$LOG" "${bins[@]}"
for t in "${bins[@]}"; do
    [ "$(cat "$LOG/$(basename "$t").rc")" = 0 ] || red+=("suite $(basename "$t") failed (exit $(cat "$LOG/$(basename "$t").rc"), $LOG/$(basename "$t").log)")
done
if [ ${#red[@]} -gt 0 ]; then printf 'RED: %s\n' "${red[@]}"; echo "log: $LOG"; exit 1; fi
echo "clean: ${#bins[@]} suite(s) left the tree as they found it (log: $LOG)"
