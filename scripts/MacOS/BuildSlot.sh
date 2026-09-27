#!/usr/bin/env bash
# BuildSlot.sh — serialise builds across parallel agent worktrees on ONE shared machine.
#
# WHY THIS EXISTS, in numbers. Eight agents each running `make -j4` put 72-80 clang processes on a
# machine with 8 performance cores: ~9x oversubscription. Nothing failed, but nothing FINISHED either
# — every agent sat waiting on a build that was being starved by the other seven. Killing builds does
# not help while the agents are alive, because each live agent simply starts another one. The fix is
# not force, it is a queue.
#
# The lock is a DIRECTORY, because mkdir is atomic on every filesystem we care about: exactly one
# waiter wins, with no race window that a test-then-create would leave open.
#
# Usage — wrap the whole build, keep your own -j and your own log:
#     scripts/MacOS/BuildSlot.sh make -C build/Projects Editor config=debug -j4 CC="ccache clang" CXX="ccache clang++"
#
# The wrapped command's exit code is passed through unchanged, so `echo $? > my_rc.txt` still works
# and still means what it meant.

set -uo pipefail

LOCK="${DESERT_BUILD_LOCK:-/tmp/desert_build.lock}"
OWNER="$LOCK/owner"
# Long enough for a full cold build of the biggest target, short enough that a crashed holder does not
# park everyone until morning.
STALE_SECONDS="${DESERT_BUILD_LOCK_STALE:-2400}"
WAITED=0

while true; do
    if mkdir "$LOCK" 2>/dev/null; then
        printf '%s\n' "$$ $(date '+%F %T') ${PWD}" > "$OWNER"
        break
    fi

    # A holder that died leaves the directory behind. Break the lock only on AGE, and only after
    # confirming the recorded pid is gone: a slow build must never be mistaken for a dead one.
    if [ -f "$OWNER" ]; then
        holder_pid=$(awk '{print $1}' "$OWNER" 2>/dev/null)
        age=$(( $(date +%s) - $(stat -f %m "$OWNER" 2>/dev/null || date +%s) ))
        if [ -n "$holder_pid" ] && ! kill -0 "$holder_pid" 2>/dev/null && [ "$age" -gt 120 ]; then
            echo "[BuildSlot] holder pid $holder_pid is gone (${age}s); breaking the lock" >&2
            rm -rf "$LOCK"
            continue
        fi
        if [ "$age" -gt "$STALE_SECONDS" ]; then
            echo "[BuildSlot] lock held ${age}s, past the ${STALE_SECONDS}s ceiling; breaking it" >&2
            rm -rf "$LOCK"
            continue
        fi
    fi

    if [ $(( WAITED % 60 )) -eq 0 ]; then
        echo "[BuildSlot] waiting for the build slot (${WAITED}s): $(cat "$OWNER" 2>/dev/null)" >&2
    fi
    sleep 5
    WAITED=$(( WAITED + 5 ))
done

# Release on every exit path, including a signal — otherwise one Ctrl-C parks every other agent.
trap 'rm -rf "$LOCK"' EXIT INT TERM

"$@"
rc=$?
exit $rc
