#!/usr/bin/env bash
# EVERY THIRD-PARTY HEADER MUST LIVE IN THIS REPOSITORY.
#
# WHY THIS EXISTS. `TextureBinary.cpp` included <fmt/format.h>. No checkout of this repository
# contains that header -- it resolved only because the author's machine had Homebrew's fmt at
# /opt/homebrew/include. It compiled here, it compiled in every local sweep, and it broke two test
# suites the first time a machine without Homebrew fmt tried: AssetEviction and AssetMissingFile,
# `fatal error: 'fmt/format.h' file not found`.
#
# The engine's own answer for formatting is spdlog's BUNDLED copy, <spdlog/fmt/fmt.h>, which every
# other translation unit already used. One file reached outside the tree and nothing said so.
#
# THE RELATION PINNED HERE is not "the count of bad includes is zero" -- that is satisfiable by
# editing a number. It is: for each forbidden root below, NO engine source may name it, and the
# header it stands for must be reachable another way inside the tree.
#
# ── WHY THE TWO HALVES ARE NOW SEPARATE (2026-09-23) ──────────────────────────────────────────────
#
# The replacement lives INSIDE A SUBMODULE (`ThirdParty/spdlog`). The job that runs this gate --
# `clang-format (changed lines)` -- checks out deliberately WITHOUT submodules and with
# `filter: blob:none`, because a full checkout once ate its entire ten-minute budget. So the
# replacement file is absent there BY DESIGN, and the old script read that absence as "BROKEN GATE",
# failed, and -- because the failure path did `continue` -- NEVER RAN THE SCAN AT ALL.
#
# It stayed that way for a day. The gate was not merely red: while red it was BLIND, and since
# Windows and macOS both `needs: format`, both were SKIPPED on every push. Three failures in one:
# no fmt check, no Windows build, no macOS build.
#
# The defect is the one this repository keeps meeting -- AN INSTRUMENT ANSWERING A QUESTION ABOUT
# THE REPOSITORY BY LOOKING AT A WORKING DIRECTORY. "I cannot see the replacement from here" and
# "the replacement does not exist" are different answers and must never share an exit code.
#
#   * the SCAN needs only our own sources, so it runs everywhere, always;
#   * the REPLACEMENT assertion needs submodule content, so it is required only where that content
#     exists -- pass --require-replacements in a job that checks out submodules. Where it is not
#     required and not present, the script says so out loud rather than passing in silence.
set -u
cd "$(dirname "$0")/../.." || exit 2

require_replacements=0
[ "${1:-}" = "--require-replacements" ] && require_replacements=1

# root  ->  what to use instead
FORBIDDEN=( "fmt/:ThirdParty/spdlog/include/spdlog/fmt/fmt.h" )

status=0
checked=0
for row in "${FORBIDDEN[@]}"; do
    root="${row%%:*}"
    replacement="${row#*:}"
    submodule="${replacement%%/include/*}"

    # ── half one: the replacement is reachable inside the tree ────────────────────────────────────
    if [ -f "$replacement" ]; then
        checked=$(( checked + 1 ))
    elif [ "$require_replacements" = 1 ]; then
        echo "BROKEN GATE: the replacement for <${root}...> is missing from the tree: $replacement"
        echo "  This job checks out submodules, so the file is genuinely absent rather than unfetched."
        echo "  The rule cannot be satisfied, so this is a gate defect, not a source defect."
        status=1
    elif [ -d "$submodule" ] && [ -n "$(ls -A "$submodule" 2>/dev/null)" ]; then
        echo "BROKEN GATE: $submodule is populated but $replacement is not in it."
        echo "  The rule cannot be satisfied, so this is a gate defect, not a source defect."
        status=1
    else
        # Not an error: this checkout deliberately has no submodule content. Say it, do not imply it.
        echo "NOTE: <${root}...>'s replacement is inside the unpopulated submodule $submodule, so it"
        echo "  cannot be verified from this checkout. The source scan below still runs and still fails"
        echo "  on a violation; the replacement is asserted by the job that passes --require-replacements."
    fi

    # ── half two: no engine source names the forbidden root. ALWAYS RUNS. ─────────────────────────
    # This is the half that catches the defect, and the half the old script skipped when it tripped
    # over its own precondition. It needs nothing but our own sources.
    hits=$(grep -rn "#include <${root}" Desert Editor Runtime \
             --include='*.cpp' --include='*.hpp' --include='*.h' 2>/dev/null \
           | grep -v '/ThirdParty/' || true)
    if [ -n "$hits" ]; then
        echo "FORBIDDEN INCLUDE ROOT <${root}...> -- it exists in no checkout of this repository."
        echo "$hits" | sed 's/^/  /'
        echo "  Use ${replacement#ThirdParty/spdlog/include/} instead."
        status=1
    fi
done

if [ "$status" = 0 ]; then
    echo "repo-only includes: OK (${#FORBIDDEN[@]} forbidden root(s) scanned, $checked replacement(s) verified here)"
fi
exit "$status"
