#!/usr/bin/env bash
# migrate.sh [--write] <paths…> — the ONLY sanctioned way to run SceneMigrator (default: --check, writes nothing).
# Rebuilds the Debug migrator first (target SceneMigrator and what it links, -j4, after any other build finishes), then
# refuses unless Ninja, asked in dry run, has nothing left to do for it (a stale Release once wiped 101k lines in 157
# files). Runs build/Bin/Debug/SceneMigrator by absolute path with DESERT_MIGRATE_VIA_SCRIPT=1. Prints the migrator's summary; exit = the migrator's code.
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
[ -f "$BINARY.exe" ] && BINARY="$BINARY.exe"

dev_configure "$LOG" || exit 2
if ! dev_build "$LOG/build.log" SceneMigrator; then
    echo "migrate.sh: building SceneMigrator FAILED; log $LOG/build.log"
    grep -m 5 -E "error|Undefined symbols" "$LOG/build.log"
    exit 2
fi

# Staleness, asked of the build's own dependency graph: after a successful build Ninja must have nothing left to do
# for the migrator (sources, every header in its depfiles, the libraries it links, the generated reflection).
[ -x "$BINARY" ] || { echo "migrate.sh: $BINARY missing after build"; exit 2; }
if ! dev_up_to_date SceneMigrator; then
    echo "migrate.sh: REFUSED — $BINARY is older than its inputs (ninja -n SceneMigrator in build/cmake/$DEV_PRESET has work left)"
    exit 2
fi

DESERT_MIGRATE_VIA_SCRIPT=1 "$BINARY" $mode "${paths[@]}" >"$LOG/migrate.log" 2>&1
rc=$?
grep -a '^SceneMigrator: ' "$LOG/migrate.log" | tail -1
fails=$(grep -ac '^FAIL' "$LOG/migrate.log")
[ "$fails" -gt 0 ] && { echo "  $fails FAIL line(s), first:"; grep -a -m 3 '^FAIL' "$LOG/migrate.log" | cut -c1-160 | sed 's/^/  /'; }
echo "  ${mode:-write} rc=$rc; log $LOG/migrate.log"
exit $rc
