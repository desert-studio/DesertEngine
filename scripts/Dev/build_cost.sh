#!/usr/bin/env bash
# build_cost.sh [out-file] — where the compile time goes, by HEADER FULL PATH (BUILD-MOD-4).
# Needs a tree configured with -DDESERT_TIME_TRACE=ON and built once: clang then leaves <object>.json traces next to
# every object, and ClangBuildAnalyzer (brew install clangbuildanalyzer) sums them. Counting includes by file NAME lied
# (several Mesh.hpp); the traces name each header by its absolute path and carry the parse seconds, not a guess.
# Prints: the time summary (frontend/backend seconds), the top-30 expensive headers (seconds, include count, path),
# and the total of their parse seconds as one line "TOTAL_TOP30_HEADER_SECONDS <n>" a before/after diff can grep.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
OUT="${1:-}"
command -v ClangBuildAnalyzer >/dev/null || { echo "build_cost.sh: ClangBuildAnalyzer not on PATH (brew install clangbuildanalyzer)" >&2; exit 2; }
grep -q '^DESERT_TIME_TRACE:BOOL=ON' "$DEV_BUILD/CMakeCache.txt" 2>/dev/null ||
    { echo "build_cost.sh: $DEV_BUILD is not configured with -DDESERT_TIME_TRACE=ON" >&2; exit 2; }
work="$(dev_logdir build-cost)"
# A trace is written only by a compile with the option on; a tree built before it was turned on has none.
n=$(find "$DEV_BUILD" -path '*.dir/*' -name '*.json' | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "build_cost.sh: no -ftime-trace .json under $DEV_BUILD — build the targets first" >&2; exit 2; }
# The analyzer reads its limits from ClangBuildAnalyzer.ini in the working directory.
cat >"$work/ClangBuildAnalyzer.ini" <<'INI'
[counts]
fileParse=10
fileCodegen=10
function=10
template=10
templateSet=10
header=30
headerChain=3
INI
ClangBuildAnalyzer --all "$DEV_BUILD" "$work/capture.bin" >"$work/all.log" 2>&1
(cd "$work" && ClangBuildAnalyzer --analyze capture.bin) >"$work/analyze.txt" 2>&1
# Keep the summary and the header table (each header line: "<path> (<ms> ms)" with "included <n> times" below it).
report() {
    sed -n '/Time summary/,/^$/p' "$work/analyze.txt"
    echo "**** Top-30 expensive headers (parse seconds total, include count, full path):"
    awk '
        /\*\*\*.*Expensive headers/ { on=1; next }
        on && /^\*\*\*/ { on=0 }
        on && /^[0-9]+ ms: / { ms=$1; sub(/^[0-9]+ ms: /, ""); path=$0; sub(/ \(included [0-9]+ times.*/, "", path)
                                inc="?"; if (match($0, /included [0-9]+ times/)) inc=substr($0, RSTART+9, RLENGTH-15)
                                printf "%9.1f s  %5s x  %s\n", ms/1000, inc, path; total+=ms }
        END { printf "TOTAL_TOP30_HEADER_SECONDS %.1f\n", total/1000 }' "$work/analyze.txt"
}
if [ -n "$OUT" ]; then report | tee "$OUT"; else report; fi
echo "(full analyzer output: $work/analyze.txt; traces: $n)" >&2
