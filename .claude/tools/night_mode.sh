#!/usr/bin/env bash
# night_mode.sh on [HH:MM] | off | status — the owner's "run the agents yourself overnight" switch (10-22).
# board_guard.py's Stop check refuses an idle lead (< 3 agents, no land_dev) until the stored end time.
F=~/.claude/night_mode
case "$1" in
  on)  t=$(date -j -f %H:%M "${2:-10:00}" +%s) || exit 2; [ "$t" -le "$(date +%s)" ] && t=$((t + 86400))
       echo "$t" > "$F"; echo "night mode until $(date -r "$t" '+%d.%m %H:%M')" ;;
  off) rm -f "$F"; echo "night mode off" ;;
  *)   [ -f "$F" ] && echo "until $(date -r "$(cat "$F")" '+%d.%m %H:%M')" || echo off ;;
esac
