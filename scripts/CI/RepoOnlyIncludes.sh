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
set -u
cd "$(dirname "$0")/../.." || exit 2

# root  ->  what to use instead (both halves are checked: the root is absent, the replacement exists)
FORBIDDEN=( "fmt/:ThirdParty/spdlog/include/spdlog/fmt/fmt.h" )

status=0
for row in "${FORBIDDEN[@]}"; do
    root="${row%%:*}"
    replacement="${row#*:}"

    if [ ! -f "$replacement" ]; then
        echo "BROKEN GATE: the replacement for <${root}...> is missing from the tree: $replacement"
        echo "  The rule cannot be satisfied, so this is a gate defect, not a source defect."
        status=1
        continue
    fi

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

[ "$status" = 0 ] && echo "repo-only includes: OK ($(( ${#FORBIDDEN[@]} )) forbidden root(s) checked, each with its replacement present)"
exit "$status"
