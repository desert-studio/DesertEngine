#!/usr/bin/env bash
# Report how much of a job's `timeout-minutes` that job actually spent, and say so LOUDLY before the
# ceiling is reached rather than after it is crossed.
#
# Usage: scripts/CI/JobBudget.sh <job-id> <start-epoch-seconds> [workflow-file]
#   e.g. scripts/CI/JobBudget.sh sanitizers "$JOB_START"
#
# ── WHY THIS EXISTS ────────────────────────────────────────────────────────────────────────────────
#
# The ceiling crossing is not the event worth reporting; it is the last event in a sequence nobody
# was shown. On 2026-09-23 the `sanitizers` job crossed its 90 minutes having spent 61, 79 and 79 on
# the three runs before it, and that approach was written down in the workflow itself ("Watch the
# margin; when it reaches Windows's 80 %+, raise it or split the job") — a note a human had to
# remember to re-read. Nothing measured it. This does.
#
# It also closes the second half of the same damage. A job killed by its own `timeout-minutes` is
# reported as CANCELLED, which is the same word GitHub uses for a run superseded by a newer push, and
# a cancellation reads as "nothing is wrong" rather than "the evidence was lost". A warning emitted
# on the run BEFORE the fatal one is the only cheap place to break that tie, because by the time the
# ceiling fires this script no longer runs — its step is cancelled with everything else.
#
# ── THE CEILING IS READ, NEVER RESTATED ────────────────────────────────────────────────────────────
#
# The number comes out of the workflow file itself, by job id. Restating `90` in a step would create
# exactly the failure this repository keeps meeting: two copies of one fact, drifting, with the gate
# reporting against the stale one. Change `timeout-minutes:` and this reports against the new value
# on the next run, with nothing else to remember.
#
# The 80 % threshold IS a constant, and one constant is unavoidable — but it is printed on every run
# next to the number it is compared with, so it is an argument on the screen rather than a rule that
# drifts in silence.
#
# ── WHY IT FAILS RATHER THAN SHRUGS ────────────────────────────────────────────────────────────────
#
# If it cannot find the job, or the job has no `timeout-minutes`, or the start time is not a number,
# it exits non-zero. A budget report that cannot find its budget has nothing to say, and this
# repository has already paid for the other behaviour once: a gate whose precondition failed, which
# reported red while checking nothing (commit e8ab18a2). Those conditions are all static — a wrong
# job id is wrong on every run, not on an unlucky one — so this cannot turn a good run red by chance.
set -uo pipefail

WARN_AT_PERCENT=80

JOB_ID="${1:?job id required, e.g. sanitizers}"
START="${2:?job start time in epoch seconds required}"
WORKFLOW="${3:-.github/workflows/ci.yml}"

case "$START" in
'' | *[!0-9]*)
    echo "[ERROR] start time '$START' is not epoch seconds. The caller must record it in the job's" >&2
    echo "        FIRST step: echo \"JOB_START=\$(date +%s)\" >> \"\$GITHUB_ENV\"" >&2
    exit 1
    ;;
esac

if [ ! -f "$WORKFLOW" ]; then
    echo "[ERROR] workflow file not found: $WORKFLOW" >&2
    exit 1
fi

# The job's own block: from `  <job-id>:` at two-space indent up to the next key at that indent.
# Deliberately narrow — a `timeout-minutes` belonging to the NEXT job would be a wrong answer that
# looks exactly like a right one.
CEILING="$(awk -v job="$JOB_ID" '
    $0 ~ "^  " job ":[[:space:]]*$" { inside = 1; next }
    inside && /^  [A-Za-z_-]+:/     { inside = 0 }
    inside && /^[[:space:]]+timeout-minutes:[[:space:]]*[0-9]+[[:space:]]*$/ {
        gsub(/[^0-9]/, "", $0); print; exit
    }
' "$WORKFLOW")"

case "$CEILING" in
'' | *[!0-9]*)
    echo "[ERROR] no numeric 'timeout-minutes' found for job '$JOB_ID' in $WORKFLOW." >&2
    echo "        Either the job id is wrong, or its ceiling is an expression this cannot read." >&2
    exit 1
    ;;
esac

NOW="$(date +%s)"
ELAPSED=$((NOW - START))
[ "$ELAPSED" -lt 0 ] && ELAPSED=0
CEILING_SEC=$((CEILING * 60))
PERCENT=$((ELAPSED * 100 / CEILING_SEC))

LINE="job '$JOB_ID': $((ELAPSED / 60))m$((ELAPSED % 60))s of ${CEILING}m — ${PERCENT} % of its ceiling (warn at ${WARN_AT_PERCENT} %)"
echo "$LINE"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    echo "- \`$JOB_ID\` used **${PERCENT} %** of its ${CEILING}-minute ceiling (${ELAPSED}s)" >>"$GITHUB_STEP_SUMMARY"
fi

if [ "$PERCENT" -ge "$WARN_AT_PERCENT" ]; then
    # ::warning:: and not ::error:: on purpose. The job's verdict belongs to the tests it ran; this is
    # a statement about next week. An annotation lands on the run summary where a person reading a
    # GREEN run still sees it, which is the only run this can usefully be read on.
    echo "::warning title=Job budget::$LINE. Split the job or reduce its work — raising the ceiling only moves this line."
fi

exit 0
