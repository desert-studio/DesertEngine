#!/bin/bash
# The one build entry (LEAD_PROTOCOL §СБОРКА, owner 2026-11-05): one build per batch of merged task branches in the
# land tree. build_gate.py decides whether this call may run; this script runs it and leaves <log>.rc for the gate.
# usage: build_land.sh <land tree> <log> [scn]
[ $# -ge 2 ] || { echo "usage: build_land.sh <land tree> <log> [scn]"; exit 2; }
rm -f "$2.rc"
# The architecture ratchet (skill desert-engine-architecture) before the batch spends an hour of compile time.
python3 "$1/scripts/Dev/arch_census.py" || { echo "build_land: the architecture census rose"; exit 2; }
export DESERT_NINJA_ARGS="-k 0"   # one build shows every compile error of the batch, not the first one
# play_scenario.py needs both Editor and DesertCtl in build/Bin, so the Mac path builds both.
if [ "$(uname)" = Darwin ]; then bash ~/.claude/tools/build_quiet.sh "$1" "$2" Editor DesertCtl
else bash C:/aftmp/build_low_run.sh "$@"; fi
rc=$?
echo "$rc" > "$2.rc"
exit $rc
