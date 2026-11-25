#!/bin/bash
# build_slot.sh — persistent build slots: worktrees whose build directory outlives the task. A new task TAKES a free slot
# and switches its branch inside it, so its first build compiles only what differs from the slot's last build; whatever
# another tree already compiled comes from the shared compile cache (ccache_launcher.sh). UE: the workspace stays and the
# stream is switched inside it (UGS); a fresh workspace per task is the exception, not the rule.
#
#   build_slot.sh take <CODE> <branch> [<base>]   the slot's tree on stdout (last line); <base> default origin/dev.
#                                                 An existing branch (local or origin) is switched to as it is.
#                                                 All slots taken and fewer than DESERT_SLOTS (4) exist: a new one.
#   build_slot.sh release <tree>                  the task is over (its commits pushed): detach, keep files and build.
#   build_slot.sh warm [<tree>...]                free slots to the origin/dev tip, Editor built (run after a land).
#   build_slot.sh list
#
# A slot is <parent of the main tree>/DesertEngine-S<n>; its holder ("<CODE> <branch> <date>") lives in the slot's git
# dir (desert-slot), never in the working tree. A slot with uncommitted changes is never taken or released.
set -u
MAIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAIN="$(git -C "$MAIN" worktree list --porcelain | sed -n '1s/^worktree //p')"
PARENT="${MAIN%/*}"
MAX="${DESERT_SLOTS:-4}"

slots() { ls -d "$PARENT"/DesertEngine-S[0-9]* 2>/dev/null | sort -V; }
lockfile() { echo "$(git -C "$1" rev-parse --absolute-git-dir)/desert-slot"; }
# Tracked edits only; " T" = a submodule the slot links to the main tree's checkout (new_tree.sh), not an edit.
dirty() { git -C "$1" status --porcelain --untracked-files=no | grep -v '^ T ' | head -5; }

# sync_submodules <tree>: every submodule at the commit the tree's HEAD pins. One the main tree has checked out at that
# very commit is a link to the main tree's checkout (no second copy on disk); any other is a real checkout. A link
# to a checkout at ANOTHER commit compiles the wrong sources (new_tree.sh linked ThirdParty/ImGui of the main tree's
# older dev: "no member named 'WaitWindowIdleFn'", BUILD-FAST-1 11-17).
sync_submodules() {
    local t="$1" mode type sha p have
    git -C "$t" ls-tree -r HEAD | while read -r mode type sha p; do
        [ "$type" = commit ] || continue
        have=$(git -C "$MAIN/$p" rev-parse HEAD 2>/dev/null)
        if [ "$have" = "$sha" ]; then
            [ -L "$t/$p" ] || { rm -rf "${t:?}/$p"; ln -s "$MAIN/$p" "$t/$p"; }
        else
            [ -L "$t/$p" ] && rm "$t/$p"
            git -C "$t" submodule update --init -q "$p"
        fi
    done
}

# new_slot <tree> <base>: a worktree ready to build -- submodules (sync_submodules), untracked ThirdParty cloned
# copy-on-write from the main tree (the shape of ~/.claude/tools/new_tree.sh).
new_slot() {
    local t="$1" base="$2" p n
    git -C "$MAIN" worktree add -q --detach "$t" "$base" || return 1
    sync_submodules "$t"
    for p in "$MAIN"/ThirdParty/*/; do n=$(basename "$p"); [ -e "$t/ThirdParty/$n" ] || cp -cR "$p" "$t/ThirdParty/$n"; done
}

# switch_to <tree> <branch> <base>: the branch as it exists (local, else origin), else a new one from <base>; then the
# submodules follow the new commit.
switch_to() {
    local t="$1" b="$2" base="$3"
    if git -C "$t" show-ref -q --verify "refs/heads/$b"; then git -C "$t" switch -q "$b"
    elif git -C "$t" show-ref -q --verify "refs/remotes/origin/$b"; then git -C "$t" switch -q -c "$b" --track "origin/$b"
    else git -C "$t" switch -q -c "$b" --no-track "$base"; fi || return 1
    sync_submodules "$t"
}

take() {
    local code="$1" branch="$2" base="${3:-origin/dev}" t="" s n
    git -C "$MAIN" fetch -q origin || { echo "build_slot: git fetch failed" >&2; return 1; }
    for s in $(slots); do
        [ -f "$(lockfile "$s")" ] && continue
        if [ -n "$(dirty "$s")" ]; then echo "build_slot: $s is free but has uncommitted edits -- skipped:" >&2; dirty "$s" >&2; continue; fi
        t="$s"; break
    done
    if [ -z "$t" ]; then
        n=$(slots | wc -l | tr -d ' ')
        [ "$n" -lt "$MAX" ] || { echo "build_slot: all $n slots taken (DESERT_SLOTS=$MAX):" >&2; list >&2; return 1; }
        t="$PARENT/DesertEngine-S$((n + 1))"
        while [ -e "$t" ]; do n=$((n + 1)); t="$PARENT/DesertEngine-S$((n + 1))"; done
        echo "build_slot: creating $t (its first build is a fresh tree's: the shared compile cache serves it)" >&2
        new_slot "$t" "$base" || { echo "build_slot: worktree add $t failed" >&2; return 1; }
    fi
    local from; from=$(git -C "$t" rev-parse --short HEAD)
    switch_to "$t" "$branch" "$base" || { echo "build_slot: cannot switch $t to $branch (checked out in another tree?)" >&2; return 1; }
    echo "$code $branch $(date '+%Y-%m-%d %H:%M')" > "$(lockfile "$t")"
    echo "build_slot: $code took $t: $from -> $(git -C "$t" rev-parse --short HEAD) ($branch); $(git -C "$t" diff --name-only "$from" HEAD | wc -l | tr -d ' ') files differ from its last checkout" >&2
    echo "$t"
}

release() {
    local t="${1%/}" up
    [ -f "$(lockfile "$t")" ] || { echo "build_slot: $t is not taken" >&2; return 1; }
    if [ -n "$(dirty "$t")" ]; then echo "build_slot: $t has uncommitted edits -- commit or drop them first:" >&2; dirty "$t" >&2; return 1; fi
    up=$(git -C "$t" rev-parse -q --verify '@{u}' 2>/dev/null)
    if [ -n "$up" ] && [ -n "$(git -C "$t" rev-list "$up..HEAD")" ]; then echo "build_slot: $t has commits not pushed to $(git -C "$t" rev-parse --abbrev-ref '@{u}')" >&2; return 1; fi
    git -C "$t" switch -q --detach && rm -f "$(lockfile "$t")" && echo "build_slot: $t free (at $(git -C "$t" rev-parse --short HEAD), build kept)" >&2
}

warm() {
    local s targets=("$@") log rc start
    [ ${#targets[@]} -gt 0 ] || targets=($(slots))
    git -C "$MAIN" fetch -q origin
    for s in "${targets[@]}"; do
        s="${s%/}"
        [ -f "$(lockfile "$s")" ] && { echo "build_slot: $s taken -- not warmed" >&2; continue; }
        [ -n "$(dirty "$s")" ] && { echo "build_slot: $s has uncommitted edits -- not warmed" >&2; continue; }
        git -C "$s" switch -q --detach origin/dev || continue
        sync_submodules "$s"
        ( source "$s/scripts/Dev/_common.sh"
          log="$(dev_logdir warm)"; start=$(date +%s)
          dev_configure "$log" "" && dev_build "$log/build.log" Editor; rc=$?
          echo "build_slot: warmed $s at $(git -C "$s" rev-parse --short HEAD): rc=$rc in $(( $(date +%s) - start ))s; log $log" >&2 )
    done
}

list() {
    local s h
    for s in $(slots); do
        h=$(cat "$(lockfile "$s")" 2>/dev/null || echo free)
        printf '%s  %-40s  %s %s  Editor:%s\n' "${s##*/}" "$h" "$(git -C "$s" rev-parse --short HEAD)" \
            "$(git -C "$s" branch --show-current)" "$(stat -f %Sm -t '%m-%d %H:%M' "$s/build/Bin/$(source "$s/scripts/Dev/_common.sh" && echo "$DEV_CONFIG")/Editor" 2>/dev/null || echo none)"
    done
}

cmd="${1:-}"; shift || true
case "$cmd" in
    take) [ $# -ge 2 ] || { echo "usage: build_slot.sh take <CODE> <branch> [<base>]" >&2; exit 2; }; take "$@" ;;
    release) release "$@" ;;
    warm) warm "$@" ;;
    list) list ;;
    *) sed -n '2,/^[^#]/p' "$0" | sed -n '/^#/s/^# \{0,1\}//p'; exit 2 ;;
esac
