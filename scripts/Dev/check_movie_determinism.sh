#!/usr/bin/env bash
# Two renders of the same short movie (64x36, 10 frames) must be byte-identical, and frame 9 must differ
# from frame 0 when the map animates. An ENGINE check (Runtime --render-movie), run on any project's map —
# the engine's own is Projects/Desert; a game (e.g. Mirage, its own repository) passes its .deproj.
# Usage: check_movie_determinism.sh <Runtime binary> <project.deproj> <map.desce, relative to the project>
set -euo pipefail
RUNTIME="${1:?usage: check_movie_determinism.sh <Runtime binary> <project.deproj> <map.desce>}"
PROJECT="${2:?usage: check_movie_determinism.sh <Runtime binary> <project.deproj> <map.desce>}"
MAP="${3:?usage: check_movie_determinism.sh <Runtime binary> <project.deproj> <map.desce>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
for run in a b; do
  "$RUNTIME" --project "$PROJECT" --render-movie "$MAP" --movie-out "$TMP/$run" --resolution 64x36 --fps 60 \
             --duration 0.1666667
done
[ "$(ls "$TMP/a" | wc -l)" -eq 10 ] || { echo "FAIL: expected 10 frames, got $(ls "$TMP/a" | wc -l)"; exit 1; }
diff -r "$TMP/a" "$TMP/b" >/dev/null || { echo "FAIL: two runs differ"; exit 1; }
cmp -s "$TMP/a/00000.png" "$TMP/a/00009.png" && { echo "FAIL: frame 9 equals frame 0 (nothing animates)"; exit 1; }
echo "OK: 10 frames, two runs byte-identical, the movie moves"
