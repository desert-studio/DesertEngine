#!/usr/bin/env bash
# migrate.sh [--write] <paths…> — the ONLY sanctioned way to run SceneMigrator (default: --check, writes nothing).
# Rebuilds the Debug migrator first (Common.make + SceneMigrator.make, -j4, after any other make finishes), then
# refuses if the binary is older than any of its inputs: Tools/SceneMigrator sources, every dependency in its .d
# files, libCommon.a (a stale Release once wiped 101k lines in 157 files). Runs build/Bin/Debug/SceneMigrator by
# absolute path with DESERT_MIGRATE_VIA_SCRIPT=1. Prints the migrator's summary; exit = the migrator's code.
set -u
source "$(dirname "$0")/_common.sh"
case "${1:-}" in -h|--help|"") dev_help "$0"; exit 0 ;; esac
mode=--check; [ "$1" = "--write" ] && { mode=""; shift; }
[ "${1:-}" = "--check" ] && shift
[ $# -gt 0 ] || { echo "migrate.sh: no paths"; exit 2; }
# Paths are the caller's; resolve them before moving to the tree root.
paths=()
for p in "$@"; do
    [ -e "$p" ] || { echo "migrate.sh: no such path: $p"; exit 2; }
    paths+=("$(cd "$(dirname "$p")" && pwd)/$(basename "$p")")
done
cd "$DEV_ROOT" || exit 2
LOG=$(dev_logdir migrate)
BINARY="$DEV_ROOT/build/Bin/Debug/SceneMigrator"
OBJ="build/Intermediates/Debug/Debug/SceneMigrator"

waited=0
while pgrep -x make >/dev/null; do sleep 10; waited=$((waited + 10)); [ $waited -ge 1800 ] && { echo "migrate.sh: another make ran 30 min"; exit 3; }; done
export CCACHE_SLOPPINESS="pch_defines,time_macros,include_file_mtime,include_file_ctime" CCACHE_COMPRESS=1 CCACHE_BASEDIR=/Users/daniilsavcenko/Desktop/Programming/C++
for mk in Common SceneMigrator; do
    if ! make -f "$mk.make" config=debug -j4 CC="ccache clang" CXX="ccache clang++" >>"$LOG/build.log" 2>&1; then
        echo "migrate.sh: building $mk FAILED; log $LOG/build.log"
        grep -m 5 -E "error:|Undefined symbols|No rule" "$LOG/build.log"
        exit 2
    fi
done

# Staleness: make decides from .d files it may not have (a fresh objdir, a moved source); check independently.
[ -x "$BINARY" ] || { echo "migrate.sh: $BINARY missing after build"; exit 2; }
newer=$( { find Tools/SceneMigrator -maxdepth 3 -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -newer "$BINARY" 2>/dev/null
           [ Tools/SceneMigrator/premake5.lua -nt SceneMigrator.make ] && echo "Tools/SceneMigrator/premake5.lua (newer than SceneMigrator.make: run premake5 gmake)"
           [ build/Bin/Debug/libCommon.a -nt "$BINARY" ] && echo build/Bin/Debug/libCommon.a
           cat "$OBJ"/*.d 2>/dev/null | tr ' \\' '\n\n' | grep -E '\.(c|cc|cpp|h|hpp|inl|glslh)$' | sort -u |
               while read -r f; do [ -f "$f" ] && [ "$f" -nt "$BINARY" ] && echo "$f"; done; } | head -5)
if [ -n "$newer" ]; then
    echo "migrate.sh: REFUSED — $BINARY is older than its inputs:"
    echo "$newer" | sed 's/^/  /'
    exit 2
fi

DESERT_MIGRATE_VIA_SCRIPT=1 "$BINARY" $mode "${paths[@]}" >"$LOG/migrate.log" 2>&1
rc=$?
grep -a '^SceneMigrator: ' "$LOG/migrate.log" | tail -1
fails=$(grep -ac '^FAIL' "$LOG/migrate.log")
[ "$fails" -gt 0 ] && { echo "  $fails FAIL line(s), first:"; grep -a -m 3 '^FAIL' "$LOG/migrate.log" | cut -c1-160 | sed 's/^/  /'; }
echo "  ${mode:-write} rc=$rc; log $LOG/migrate.log"
exit $rc
