#!/usr/bin/env bash
# Two renders of the same short movie (64x36, 10 frames) must be byte-identical, and frame 9 must differ
# from frame 0 when the map animates. Usage: check_movie_determinism.sh <Runtime binary> <map.desce>
set -euo pipefail
RUNTIME="$1"; MAP="$2"; TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
for run in a b; do
  "$RUNTIME" --render-movie "$MAP" --movie-out "$TMP/$run" --resolution 64x36 --fps 60 --duration 0.1666667
done
[ "$(ls "$TMP/a" | wc -l)" -eq 10 ] || { echo "FAIL: expected 10 frames, got $(ls "$TMP/a" | wc -l)"; exit 1; }
diff -r "$TMP/a" "$TMP/b" >/dev/null || { echo "FAIL: two runs differ"; exit 1; }
cmp -s "$TMP/a/00000.png" "$TMP/a/00009.png" && { echo "FAIL: frame 9 equals frame 0 (nothing animates)"; exit 1; }
echo "OK: 10 frames, two runs byte-identical, the movie moves"
