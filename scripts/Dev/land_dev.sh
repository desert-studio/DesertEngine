#!/usr/bin/env bash
# land_dev.sh <tree> <branch>... — land finished task branches into dev in one call (the lead's merge recipe).
# In <tree> (a clean worktree that no agent uses): fetch; a temporary branch from origin/dev; merge each origin/<branch>
# with --no-ff; .claude taken from origin/dev (agents cannot commit it); refuse symlinked submodules (ls-tree 120000)
# and added Legacy/Compat code; configure + build Editor (Debug, scripts/Dev/_common.sh); push to dev only as a fast-forward of the dev it was built on;
# regenerate CODEMAP; delete the landed remote branches.
# Exit 0 = landed. 1 = conflicts (merge left in progress in <tree>, listed). 2 = refused or build red (nothing pushed).
# Run it in the background: the build takes up to ~20 min. CI is not waited for.
set -u
[ $# -ge 2 ] || { sed -n 2,8p "$0"; exit 2; }
TREE="$1"; shift
cd "$TREE" || exit 2
# Agent trees carry symlinked submodules (new_tree.sh): a typechange (T) is the tree's layout, not a change.
[ -z "$(git diff --name-status HEAD -- . ':!.cache' | grep -v '^T')" ] || { echo "land_dev: $TREE has modified tracked files"; exit 2; }
git fetch -q origin || { echo "land_dev: fetch failed"; exit 2; }
base=$(git rev-parse origin/dev)
tmp="land/$(date +%Y%m%d-%H%M%S)"
git switch -q -c "$tmp" "$base" || exit 2
for b in "$@"; do
    git rev-parse -q --verify "origin/$b" >/dev/null || { echo "land_dev: no origin/$b"; exit 2; }
    if ! git merge -q --no-ff --no-edit -m "merge: $b into dev" "origin/$b" >/dev/null 2>&1; then
        echo "land_dev: CONFLICTS merging $b (in progress on $tmp in $TREE):"
        git diff --name-only --diff-filter=U | head -15 | sed 's/^/  /'
        exit 1
    fi
done
if ! git diff --quiet "$base" HEAD -- .claude; then
    git checkout -q "$base" -- .claude && git commit -q -m "chore: .claude equals dev"
fi
links=$(git ls-tree -r HEAD | grep -c '^120000' || true)
links_base=$(git ls-tree -r "$base" | grep -c '^120000' || true)
[ "$links" -le "$links_base" ] || { echo "land_dev: $((links - links_base)) new symlink(s) in the tree (submodules committed as links?)"; exit 2; }
legacy=$(git diff "$base" HEAD -U0 -- '*.cpp' '*.hpp' '*.h' | grep -E '^\+.*\b(Legacy|Compat)[A-Za-z_]*\b' | head -5)
[ -z "$legacy" ] || { echo "land_dev: added Legacy/Compat code — not landing:"; echo "$legacy"; exit 2; }
# The merged tree's own build scripts: the preset and the build are the ones being landed.
source "$TREE/scripts/Dev/_common.sh"
LOG=$(dev_logdir land)
dev_configure "$LOG" || { echo "land_dev: configure failed, nothing pushed"; exit 2; }
dev_build "$LOG/build.log" Editor ||
    { echo "land_dev: Editor build red, nothing pushed; log $LOG/build.log"; grep -m 15 -E "error" "$LOG/build.log"; exit 2; }
git fetch -q origin
[ "$(git rev-parse origin/dev)" = "$base" ] || { echo "land_dev: dev moved while building — re-run"; exit 2; }
git push -q origin "HEAD:dev" || { echo "land_dev: push refused"; exit 2; }
python3 .claude/tools/gen_codemap.py >/dev/null 2>&1 && ! git diff --quiet -- .claude/CODEMAP.md && \
    git commit -q -m "chore(.claude): CODEMAP after landing $*" -- .claude/CODEMAP.md && git push -q origin "HEAD:dev"
for b in "$@"; do git push -q origin --delete "$b" 2>/dev/null; done
echo "land_dev: landed $* on dev at $(git rev-parse --short HEAD) (was $(git rev-parse --short "$base"))"
exit 0
