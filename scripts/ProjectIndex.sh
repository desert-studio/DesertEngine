#!/usr/bin/env bash
# A map of this repository that CANNOT go stale, because nothing here is written down — every line is
# derived from the tree at the moment you run it.
#
# WHY IT IS GENERATED AND NOT A DOCUMENT. This project has paid three times in one day for maps that
# described a tree which had moved: `.claude/handover/*.json` was stale the day it was written and sent
# an agent at a task closed a week earlier; `Docs/UI_ROADMAP.md` said "Introspection — we have none"
# about a shipped subsystem; a brief's `Components.hpp:1373` had drifted to 1520 within hours. A written
# map is a fourth document to keep true. This one has no truth to keep.
#
# WHAT IT IS FOR: orientation, so an agent spends its budget on what the code DOES rather than on where
# things live. It answers "where" and never "why" — the why lives in the header of the file itself,
# which is where this project keeps its reasoning.
#
#   scripts/ProjectIndex.sh            everything
#   scripts/ProjectIndex.sh suites     just the test suites
#   scripts/ProjectIndex.sh components just the reflected/serialized components
#   scripts/ProjectIndex.sh gates      how to run every gate, with the exit codes that matter
set -uo pipefail
cd "$(dirname "$0")/.."
WHAT="${1:-all}"

section() { printf '\n== %s ==\n' "$1"; }

if [ "$WHAT" = all ] || [ "$WHAT" = suites ]; then
    section "TEST SUITES (from the root Makefile's own PROJECTS, never from *.make)"
    # *.make would include orphans premake never deletes; that cost a false BUILD-FAIL and, in the
    # tidy gate, a whole dead run.
    if [ -f Makefile ]; then
        TMPLIST="$(mktemp -t projectindex)"
        TOOLS="|$(ls Tools | tr '\n' '|')"
        sed -n 's/^PROJECTS := //p' Makefile | tr ' ' '\n' | while read -r p; do
            [ -n "$p" ] || continue
            case "$p" in Desert|Common|Editor|Runtime|GLFW|ImGui*|imgui-node-editor|yaml-cpp|Jolt|Lua|Optick|MeshOptimizer|Dlib|ReflectCpp|Assimp|GoogleTest|BuildAllTests|RunAllTests) continue;; esac
            case "$TOOLS" in *"|$p|"*) continue;; esac
            printf '%s\n' "$p"
        done | sort > "$TMPLIST"
        column -c 110 < "$TMPLIST" 2>/dev/null || cat "$TMPLIST"
        # THE COUNT IS DERIVED FROM THE LIST THAT WAS JUST PRINTED, and that is the whole point.
        # It used to count the RAW `PROJECTS :=` line while the list above was filtered — 263 against
        # 233 — so the header said "count" and answered a different question, with no word of it in
        # the output. An agent read 263, compared it with its sweep's 233 and had to work out which
        # of the two was lying. Found 2026-09-16 by А11.
        printf '\n  count: %s   (test suites only — libraries, Tools/ and the aggregate targets are excluded)\n' "$(grep -c . "$TMPLIST")"
        rm -f "$TMPLIST"
    else
        echo "  no Makefile — run: CI=true premake5 gmake"
    fi
fi

if [ "$WHAT" = all ] || [ "$WHAT" = components ]; then
    section "COMPONENTS THAT SERIALIZE (registered in ComponentRegistry.cpp)"
    grep -ohE '"[A-Za-z0-9_]+"' Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp 2>/dev/null \
        | tr -d '"' | sort -u | column -c 110 2>/dev/null || true

    section "FORMAT VERSION COUNTERS (each is its own sequence — never assume one)"
    grep -rnE 'inline constexpr int k[A-Za-z]*Version' Desert/Desert/Source 2>/dev/null \
        | sed 's|^|  |'
    echo "  (a file with no counter of its own does NOT borrow another's; ask before inventing one)"
fi

if [ "$WHAT" = all ] || [ "$WHAT" = gates ]; then
    section "GATES, and the exit codes that are the interface"
    cat <<'TXT'
  build       make Desert config=debug -j8 ; make Editor config=debug -j8
              new files first: CI=true premake5 gmake

  sweep       recipe in .claude/skills/desert-engine-verify §3, verbatim
              detector is the "[  FAILED  ]" marker AND the exit code, never `grep FAILED`

  format      PATH="/opt/homebrew/opt/llvm@18/bin:$PATH" ./scripts/CI/CheckFormat.sh <merge-base>
              0 clean · 1 violations · 2 THE GATE COULD NOT RUN (wrong formatter version)

  analysis    PATH="/opt/homebrew/opt/llvm@18/bin:$PATH" ./scripts/CI/CheckTidy.sh <merge-base>
              0 clean · 1 findings · 2 THE GATE COULD NOT RUN
              clang-tidy --fix is FORBIDDEN here: measured destructive

  coverage    scripts/CI/UnreachedSources.sh   (85% of TUs are compiled by no suite; check yours)
TXT
fi

if [ "$WHAT" = all ]; then
    section "SEAMS WORTH NOT BREAKING (derived: suites that link the canvas walk without Vulkan)"
    for t in UIIntrospection UICanvasContext UIEventRouting UIListView; do
        [ -d "Desert/Tests/Engine/$t" ] && printf '  %s\n' "$t"
    done
    echo "  these compile Engine/UI/UICanvasRenderer2D with NO Vulkan in the binary."
    echo "  they fail at LINK if the walk names a concrete backend — that is the boundary itself."

    section "WHERE THE REASONING LIVES"
    cat <<'TXT'
  Not here. Each file's header carries why it is shaped the way it is, and those headers are the
  only account that cannot drift from the code beside them. Read the header before the body.

  Programme-level: Docs/Animation/06_gap_analysis.md §6 (ordered plan), Docs/UI_ROADMAP.md,
  Docs/*/VERIFICATION.md (measured protocols, exact cameras, noise floors).
TXT
fi
