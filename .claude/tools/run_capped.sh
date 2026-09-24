#!/bin/bash
# Runs an editor/runtime with a memory ceiling on its phys_footprint (macOS ignores `ulimit -v`). 2026-09-24: one editor grew to
# 13.7 GB of 16 in 91 s, WindowServer starved and the kernel watchdog panicked the machine.
# Usage: run_capped.sh <command...>   Ceiling: DESERT_MEM_CAP_MB (default 6144).
CAP_MB=${DESERT_MEM_CAP_MB:-6144}
"$@" &
PID=$!
PEAK=0
while kill -0 $PID 2>/dev/null; do
  # phys_footprint, not RSS: GPU (Metal/MoltenVK) allocations live in unified memory and are invisible to
  # `ps -o rss` -- the 13.4 GB editor of 2026-09-24 08:23 passed an RSS cap untouched.
  RSS_KB=$(footprint -p $PID 2>/dev/null | awk '/phys_footprint:/{v=$2; u=$3; if(u=="GB")v*=1048576; else if(u=="MB")v*=1024; printf "%d", v; exit}')
  [ -n "$RSS_KB" ] && [ "$RSS_KB" -gt "$PEAK" ] && PEAK=$RSS_KB
  if [ -n "$RSS_KB" ] && [ "$RSS_KB" -gt $((CAP_MB * 1024)) ]; then
    kill -9 $PID 2>/dev/null
    echo "[run_capped] KILLED: resident memory $((RSS_KB / 1024)) MB > cap ${CAP_MB} MB. This is a defect to" \
         "report (which scene, which step), not a limit to raise." >&2
    wait $PID 2>/dev/null
    exit 137
  fi
  sleep 1
done
wait $PID
RC=$?
echo "[run_capped] exit=$RC peak=$((PEAK / 1024)) MB" >&2
exit $RC
