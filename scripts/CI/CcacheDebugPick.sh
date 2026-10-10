#!/usr/bin/env bash
# CcacheDebugPick.sh <debug_dir> <out_dir>
#
# Copies the ccache debug files (.ccache-input-text, .ccache-log) of TWO objects out of a
# CCACHE_DEBUGDIR tree into <out_dir>, for the workflow_dispatch input `ccache_debug` in ci.yml:
#   - one object of project Common (a unity_*.cpp object where the build is unity, i.e. Windows),
#   - one object of project Desert.
# The choice is the first in sorted path order, so two runs on the same commit pick the same objects
# and their .ccache-input-text files can be diffed line by line. Object paths are matched on the
# project's own intermediate directory segment (/Common/, /Desert/).
#
# A tree with no debug files, or no object of either project, is an error that prints what the tree
# does hold: an empty artifact would read as "nothing differs".
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <debug_dir> <out_dir>" >&2
    exit 2
fi
src="$1"
out="$2"
if command -v cygpath >/dev/null 2>&1; then
    src="$(cygpath -u "$src")"
    out="$(cygpath -u "$out")"
fi
if [ ! -d "$src" ]; then
    echo "::error::ccache debug directory $src does not exist: CCACHE_DEBUGDIR was not in effect" >&2
    exit 1
fi

logs="$(cd "$src" && find . -type f -name '*.ccache-log' | sed 's|^\./||' | LC_ALL=C sort)"
if [ -z "$logs" ]; then
    echo "::error::no *.ccache-log under $src: ccache wrote no debug files" >&2
    exit 1
fi
echo "$(printf '%s\n' "$logs" | wc -l | tr -d ' ') objects carry ccache debug files"

# Object stem: the debug file name minus ".<timestamp>.ccache-log".
stems="$(printf '%s\n' "$logs" | sed -E 's/\.[0-9_]+\.ccache-log$//' | LC_ALL=C sort -u)"

common_all="$(printf '%s\n' "$stems" | grep -E '(^|/)Common/' || true)"
common="$(printf '%s\n' "$common_all" | grep -E '/unity_[^/]*$' || true)"
[ -n "$common" ] || common="$common_all"
desert="$(printf '%s\n' "$stems" | grep -E '(^|/)Desert/' | grep -vE '(^|/)Common/' || true)"

fail=0
for name in Common Desert; do
    if [ "$name" = Common ]; then list="$common"; else list="$desert"; fi
    if [ -z "$list" ]; then
        echo "::error::no object of project $name among the ccache debug files" >&2
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    echo "First 40 objects found:" >&2
    printf '%s\n' "$stems" | head -40 >&2
    exit 1
fi

mkdir -p "$out"
for stem in "$(printf '%s\n' "$common" | head -1)" "$(printf '%s\n' "$desert" | head -1)"; do
    echo "picked: $stem"
    dest="$out/$(dirname "$stem")"
    mkdir -p "$dest"
    base="$(basename "$stem")"
    found=0
    for f in "$src/$(dirname "$stem")/$base".*.ccache-input-text "$src/$(dirname "$stem")/$base".*.ccache-log; do
        [ -f "$f" ] || continue
        cp "$f" "$dest/"
        found=$((found + 1))
    done
    if [ "$found" -eq 0 ]; then
        echo "::error::$stem has no .ccache-input-text/.ccache-log files" >&2
        exit 1
    fi
done
find "$out" -type f | LC_ALL=C sort
