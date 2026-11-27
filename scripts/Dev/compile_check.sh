#!/bin/bash
# compile_check.sh [<target>...] — does this tree's branch compile? Prints OK or the first errors (file:line relative
# to the tree); exit 0 / 1. Compiles every object the targets (default: Editor) are linked from -- no archive, no link
# -- in two passes: first the branch's own sources (changed against the merge base with origin/dev, plus uncommitted
# edits), so their errors arrive within a minute; then every other object the edits made stale (a changed header's
# includers). Up-to-date objects are skipped, the rest mostly come from the shared compile cache (buildpool_enable).
# Runs as one build of the machine's build pool (scripts/Dev/pool.sh run): beside other trees' builds, never queued.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
case "${1:-}" in -h|--help) dev_help "$0"; exit 0 ;; esac
TARGETS=("$@"); [ ${#TARGETS[@]} -gt 0 ] || TARGETS=(Editor)
LOG="$(dev_logdir compile_check)"
start=$(date +%s)
dev_configure "$LOG" "" || exit 1
dev_drop_foreign_pch
cd "$DEV_BUILD" || exit 1

ninja -t inputs "${TARGETS[@]}" 2>"$LOG/inputs.err" | grep -E '\.(o|obj)$' > "$LOG/objects.txt"
[ -s "$LOG/objects.txt" ] || { echo "compile_check: no objects for ${TARGETS[*]} (ninja -t inputs: $(head -1 "$LOG/inputs.err"))"; exit 1; }
# The branch's own sources -> their DEV_CONFIG objects (compile_commands.json: file -> output), only those the targets use.
base=$(git -C "$DEV_ROOT" merge-base HEAD origin/dev 2>/dev/null || git -C "$DEV_ROOT" rev-parse HEAD)
{ git -C "$DEV_ROOT" diff --name-only "$base"; git -C "$DEV_ROOT" ls-files --others --exclude-standard; } | sort -u > "$LOG/changed.txt"
python3 - "$DEV_ROOT" "$LOG/changed.txt" "$LOG/objects.txt" "$DEV_CONFIG" > "$LOG/own.txt" <<'EOF'
import json, os, sys
root, changed, objects, config = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
mine = {os.path.join(root, l.strip()) for l in open(changed) if l.strip()}
wanted = {l.strip() for l in open(objects)}
for e in json.load(open('compile_commands.json')):
    out = e.get('output', '')
    if '/' + config + '/' in out and os.path.normpath(os.path.join(e['directory'], e['file'])) in mine and out in wanted:
        print(out)
EOF

run_pass() {  # run_pass <name> <object list file>; ninja's -j is the pool's window (pool.sh run sets it)
    [ -s "$2" ] || return 0
    bash "$DEV_ROOT/scripts/Dev/pool.sh" run --label "compile_check $1" --\
        sh -c 'xargs ninja -k 0 -j "$CMAKE_BUILD_PARALLEL_LEVEL" < "$1"' _ "$2" > "$LOG/$1.log" 2>&1
}
report() {  # report <pass log>: the first errors, paths relative to the tree
    echo "compile_check: FAILED ($1, $(( $(date +%s) - start ))s; log $LOG/$1.log). First errors:"
    grep -E '(error|fatal error):|^FAILED: ' "$LOG/$1.log" | grep -v '^FAILED: ' | sed -E 's#^(\.\./)+##' | awk '!seen[$0]++' | head -15
    echo "objects that failed: $(grep -c '^FAILED: ' "$LOG/$1.log")"
}
run_pass own "$LOG/own.txt" || { report own; exit 1; }
run_pass all "$LOG/objects.txt" || { report all; exit 1; }
compiled=$(cat "$LOG/own.log" "$LOG/all.log" 2>/dev/null | grep -cE '^\[[0-9]+/[0-9]+\] Building')
echo "compile_check: OK ${TARGETS[*]}: $compiled object(s) compiled ($(wc -l < "$LOG/own.txt" | tr -d ' ') of the branch's own first), $(wc -l < "$LOG/objects.txt" | tr -d ' ') checked, $(( $(date +%s) - start ))s; log $LOG"
