#!/bin/bash
# The one build entry (LEAD_PROTOCOL §СБОРКА, owner 2026-11-05): one build per batch of merged task branches in the
# land tree. build_gate.py decides whether this call may run; this script runs it and leaves <log>.rc for the gate.
# usage: build_land.sh <land tree> <log> [scn]
[ $# -ge 2 ] || { echo "usage: build_land.sh <land tree> <log> [scn]"; exit 2; }
rm -f "$2.rc"
export CCACHE_RECACHE=1
bash C:/aftmp/build_low_run.sh "$@"
rc=$?
echo "$rc" > "$2.rc"
exit $rc
