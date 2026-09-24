#!/bin/bash
# Runs an editor/runtime with a memory ceiling (macOS ignores `ulimit -v`). 2026-09-24: one editor grew to
# 13.7 GB of 16 in 91 s, WindowServer starved and the kernel watchdog panicked the machine.
# Usage: run_capped.sh <command...>   Ceiling: DESERT_MEM_CAP_MB (default 6144).
CAP_MB=${DESERT_MEM_CAP_MB:-6144}
"$@" &
PID=$!
PEAK=0
while kill -0 $PID 2>/dev/null; do
  RSS_KB=$(ps -o rss= -p $PID 2>/dev/null | tr -d ' ')
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
