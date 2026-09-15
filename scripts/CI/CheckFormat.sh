#!/usr/bin/env bash
# Diff-based clang-format gate: only the lines CHANGED vs the base must satisfy .clang-format.
# Deliberately NOT a full-tree check — a large share of the codebase predates the config, so a
# whole-repo --Werror would fail on legacy noise forever. The rule this enforces instead:
# code you TOUCH becomes clean; the tree converges over time.
#
# Usage: scripts/CI/CheckFormat.sh [<base-ref-or-sha>]
#   CI passes the PR base sha (pull_request) or the pre-push sha (push);
#   locally, run it bare to check your work against origin/dev.
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE FORMATTER IS NAMED AND ITS VERSION IS PRINTED, and both of those are fixes for defects this
# gate actually had on 2026-09-08.
#
# 1. IT WAS UNPINNED. The workflow installed the unversioned `clang-format` package, i.e. whatever the
#    runner image shipped, while every developer verifies with Homebrew's llvm@18 — so the gate's
#    verdict changed silently when GitHub rebuilt the image, and it was not comparable to anything a
#    human could run. A five-task merge whose changed lines were clean under 18 was rejected here.
#
# 2. WORSE: IT COULD NOT TELL "I FOUND VIOLATIONS" FROM "I COULD NOT RUN". `git clang-format` was
#    invoked with `|| true` and ANY unrecognised output was reported as violations — so when the
#    binary was missing, git printed "clang-format is not a git command" and this script called that
#    a formatting failure. The fix attempt for (1) hit exactly that and produced a red run whose
#    message pointed at the wrong thing entirely. That is this project's most frequent defect shape:
#    an instrument answering a different question than the one asked, with nothing in its output to
#    say so — and here it was the gate itself.
#
# The consequence is not cosmetic either way: this job GATES Windows and macOS, so a wrong red here
# SKIPS both, and a batch lands with no platform evidence at all.
# TWO BINARIES ARE NEEDED, NOT ONE, and conflating them cost a third red run. `clang-format` is the
# formatter; `git-clang-format` is a separate Python wrapper that computes the changed-line ranges and
# calls it. Debian ships them under versioned names (clang-format-18, git-clang-format-18), and there
# is NO unversioned alias — so `git clang-format` is not a git subcommand at all on a runner, however
# correctly `--binary` names the formatter. The wrapper is invoked DIRECTLY here for that reason.
#
# "PINNED TO 18" IS NOT PINNED ENOUGH, AND THE PATCH VERSION DECIDES. Measured 2026-09-15, one push
# after the paragraphs above were written: CI runs **Ubuntu clang-format 18.1.3** while every
# developer here runs **Homebrew 18.1.8**, and the two DISAGREE. `Tools/DesertHeaderTool/main.cpp`
# passed this gate locally -- twice, run verbatim, against the same base -- and failed it on CI, which
# reported a violation in code the local formatter had just called clean. 18.1.3 insists on packing
# adjacent string literals in an `operator<<` chain onto fewer lines; 18.1.8 accepts either form and
# so has nothing to say about it.
#
# That asymmetry is the usable rule: the STRICTER version is the one that gates, and it is not the one
# anybody here can run. A local pass is therefore NECESSARY AND NOT SUFFICIENT, and the failure it
# hides is the most expensive kind available -- this job gates Windows and macOS, so both were SKIPPED
# and the push bought no platform evidence at all.
#
# This is the same defect the version pin below was written to close (2026-09-08: an unpinned
# formatter gave the developer and CI different verdicts on identical code), surviving one digit
# further down. Closing it properly means pinning the PATCH version on both sides -- a decision about
# what CI installs, not a line in this script -- so it is recorded here rather than papered over.
# Until then: when this gate fails on CI and passes locally, believe CI and apply the diff it printed.
# Both versions accepted the result in the one case measured, so they do not oscillate.

CF=""
for candidate in clang-format-18 clang-format; do
    if command -v "$candidate" >/dev/null 2>&1; then CF="$candidate"; break; fi
done
GITCF=""
for candidate in git-clang-format-18 git-clang-format; do
    if command -v "$candidate" >/dev/null 2>&1; then GITCF="$candidate"; break; fi
done
if [ -z "$CF" ] || [ -z "$GITCF" ]; then
    echo "clang-format: MISSING TOOL — formatter='${CF:-not found}' wrapper='${GITCF:-not found}'." >&2
    echo "Looked for clang-format-18/clang-format and git-clang-format-18/git-clang-format." >&2
    echo "This is an environment failure, NOT a formatting violation — do not go looking at the diff." >&2
    exit 2
fi
echo "clang-format gate using: $("$CF" --version)  (wrapper: $GITCF)"

BASE_INPUT="${1:-origin/dev}"

# Resolve a usable base: the given ref, else HEAD~1 (first pushes send an all-zero 'before' sha).
if git rev-parse --verify -q "$BASE_INPUT^{commit}" >/dev/null 2>&1; then
    BASE=$(git merge-base HEAD "$BASE_INPUT" 2>/dev/null || echo "$BASE_INPUT")
else
    BASE=$(git rev-parse HEAD~1 2>/dev/null || git rev-parse HEAD)
fi

# The exit STATUS is captured separately from the output, because the two answer different questions:
# git-clang-format exits non-zero both for "reformatted something" and for "I broke", and only the
# output can tell those apart. Anything that is neither a clean report nor a diff is an environment
# failure and exits 2 — a code no formatting violation can produce.
set +e
OUT=$("$GITCF" --binary "$CF" --diff "$BASE" -- '*.cpp' '*.hpp' 2>&1)
RC=$?
set -e

if [ -z "$OUT" ] || echo "$OUT" | grep -qE "(no modified files to format|did not modify any files)"; then
    echo "clang-format: changed lines are clean (vs $BASE)"
    exit 0
fi

if ! echo "$OUT" | grep -q '^diff --git '; then
    echo "$OUT" >&2
    echo "" >&2
    echo "clang-format: the gate FAILED TO RUN (exit $RC) — the output above is not a diff." >&2
    echo "This is an environment failure, NOT a formatting violation." >&2
    exit 2
fi

echo "$OUT"
echo ""
echo "clang-format violations in the lines you changed."
echo "Fix locally with:"
echo "  /opt/homebrew/opt/llvm@18/bin/git-clang-format --binary /opt/homebrew/opt/llvm@18/bin/clang-format $BASE"
echo "(then review + commit the touch-ups; local Homebrew clang-format is v22 and disagrees with 18)"
exit 1
