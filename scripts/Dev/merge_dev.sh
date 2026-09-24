#!/usr/bin/env bash
# merge_dev.sh [base=origin/dev] — merge the latest base into the current task branch, in one call.
# Refuses on a dirty tree or on dev/main itself; fetches, merges (no editor). On conflicts: lists the
# conflicted files and stops with the merge in progress (resolve, or `git merge --abort`), exit 1.
# After a clean merge, if .claude differs from base (an agent cannot commit it), says so and exits 2 —
# the lead must fix that copy; nothing is checked out or staged. Exit 0 = merged, .claude equals base.
set -u
source "$(dirname "$0")/_common.sh"
[ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] && { dev_help "$0"; exit 0; }
BASE="${1:-origin/dev}"
cd "$DEV_ROOT" || exit 2
branch=$(git symbolic-ref --short -q HEAD) || { echo "merge_dev: detached HEAD"; exit 2; }
case "$branch" in dev|main) echo "merge_dev: refusing to merge into $branch"; exit 2 ;; esac
git diff --quiet HEAD -- . ':!.cache' || { echo "merge_dev: tracked files modified — commit or stash first"; exit 2; }
LOG=$(dev_logdir merge)
remote="${BASE%%/*}"; [ "$remote" != "$BASE" ] && { git fetch -q "$remote" >"$LOG/fetch.log" 2>&1 || { echo "merge_dev: fetch $remote failed; log $LOG/fetch.log"; exit 2; }; }
before=$(git rev-parse --short HEAD)
if git merge --no-edit "$BASE" >"$LOG/merge.log" 2>&1; then
    after=$(git rev-parse --short HEAD)
    if [ "$before" = "$after" ]; then echo "merge_dev: $branch already contains $BASE ($after)"
    else echo "merge_dev: merged $BASE into $branch: $before -> $after ($(git diff --shortstat "$before" HEAD | sed 's/^ //'))"; fi
else
    conflicts=$(git diff --name-only --diff-filter=U)
    if [ -z "$conflicts" ]; then echo "merge_dev: merge failed without conflicts; log $LOG/merge.log"; tail -3 "$LOG/merge.log"; exit 2; fi
    echo "merge_dev: CONFLICTS in $(echo "$conflicts" | wc -l | tr -d ' ') file(s); merge left in progress (git merge --abort to undo):"
    echo "$conflicts" | head -15 | sed 's/^/  /'
    exit 1
fi
if ! git diff --quiet "$BASE" HEAD -- .claude; then
    echo "merge_dev: .claude differs from $BASE, lead must fix:"
    git diff --stat "$BASE" HEAD -- .claude | tail -6 | sed 's/^/  /'
    exit 2
fi
exit 0
