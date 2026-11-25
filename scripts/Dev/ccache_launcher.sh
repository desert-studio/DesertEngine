#!/bin/bash
# ccache_launcher.sh <ccache> <compiler> <args...> — the compiler launcher of every compile on macOS/Linux
# (CMakeLists.txt: CMAKE_<LANG>_COMPILER_LAUNCHER = this script + ccache). One compile cache SHARED BY EVERY TREE
# (DesertEngine-<X> worktrees, build slots): the role of UE's shared compile cache / DDC, which serves every workspace.
#
#   base_dir  the folder holding the trees: absolute paths under it reach the compiler relative to the build directory,
#             so the same compile in two trees hashes the same.
#   PCH       clang writes the absolute paths of the tree that built it into a precompiled header, and ccache hashes the
#             PCH a source includes: before BUILD-FAST-1 every source behind a PCH (all of Common, the engine modules and
#             the editor libraries) missed in every tree but the one that filled the cache (38 % hits, 11-17).
#             With pch_external_checksum (CCACHE_PCH_EXTSUM) ccache hashes <pch>.sum instead, which this script writes after every
#             PCH compile: a digest of what the PCH is made of with the tree root taken out -- its command line and every
#             file its depfile lists (path relative to the tree, content). Same headers + same flags = same .sum in every
#             tree. -fno-pch-timestamp (CMakeLists.txt) keeps header mtimes, which differ per tree, out of the PCH.
#             The PCH ITSELF never crosses trees: its compile runs without base_dir, so the cache key holds this tree's
#             absolute paths. With base_dir a Release PCH compile hashed the same in every tree (no -g, so no cwd in the
#             hash) and the cache handed DesertEngine-LAND the PCH DesertEngine-S1 had compiled (11-25): its headers were
#             S1's files, #pragma once did not recognise LAND's copy of UUID.hpp, "redefinition of 'UUID'". Only the
#             consumers share, through <pch>.sum. A PCH a tree got before this rule: _common.sh dev_drop_foreign_pch.
#   settings  here, not in the caller's environment, so every build of the tree (build_quiet.sh, dev_build, an IDE,
#             a bare `ninja`) caches alike. CCACHE_STATSLOG passes through: compile_check.sh / timings read hit rates from it.
set -u
ROOT="${BASH_SOURCE[0]%/scripts/Dev/ccache_launcher.sh}"
export CCACHE_BASEDIR="${CCACHE_BASEDIR:-${ROOT%/*}}"
export CCACHE_SLOPPINESS="pch_defines,time_macros,include_file_mtime,include_file_ctime,locale" CCACHE_PCH_EXTSUM=true

out=""; dep=""; prev=""
for a in "$@"; do
    case "$prev" in -o) out="$a" ;; -MF) dep="$a" ;; esac
    prev="$a"
done
case "$out" in *.pch) ;; *) exec "$@" ;; esac

# A PCH: its old digest goes first, so a failed or interrupted compile never leaves a .sum describing another PCH.
rm -f "$out.sum"
CCACHE_BASEDIR= "$@" || exit $?
[ -n "$dep" ] && [ -f "$dep" ] || { echo "ccache_launcher: $out has no depfile (-MF), its consumers will not be cached across trees" >&2; exit 0; }
# The depfile: "<out>: dep dep \" lines; our paths hold no spaces (a space-escaped path would split, and change the digest).
deps=$(sed -e 's/\\$//' "$dep" | tr ' ' '\n' | grep -v -e '^$' -e ':$' | sort -u)
{
    printf '%s\n' "$@"
    printf '%s\n' "$deps"
    printf '%s\n' "$deps" | tr '\n' '\0' | xargs -0 cat
} | sed "s#$ROOT#@ROOT@#g" | shasum -a 256 | cut -d' ' -f1 > "$out.sum.tmp" && mv "$out.sum.tmp" "$out.sum"
