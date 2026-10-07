#!/usr/bin/env bash
# Builds the three startup movies with ONE command: the engine renders each movie map (Runtime
# --render-movie: offscreen 3840x2160, fixed 1/60 s step), the sound is the authored <name>.wav beside this
# script (48 kHz stereo, exactly the movie's length, its own fade-out), and ffmpeg encodes AV1 (SVT-AV1,
# 10-bit 4:2:0, Rec.709 tv range) + Opus into WebM.
# Usage: build_movies.sh <Runtime binary> [movie name...]   (no names = all three)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$(cd "$HERE/.." && pwd)"; PROJECT="$(cd "$HERE/../../.." && pwd)"
RUNTIME="${1:?usage: build_movies.sh <Runtime binary> [movie name...]}"; shift
ONLY=" $* "
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
FPS=60; RES=3840x2160

# name | map (relative to the project) | seconds   (sound: Source/<name>.wav)
MOVIES=(
  "DesertEngine|Content/Movies/Source/DesertEngine.desce|3.0"
  "DesertStudio|Content/Movies/Source/DesertStudio.desce|3.0"
  "Title|Content/Movies/Source/Title.desce|4.0"
)

for entry in "${MOVIES[@]}"; do
  # NOT "SECONDS": that is bash's clock (an assigned "3.0" reads back as "0", an assigned "3" as 3 plus the
  # seconds elapsed since, so the sound length after a render was wrong too).
  IFS='|' read -r NAME MAP DURATION <<<"$entry"
  [[ "$ONLY" == "  " || "$ONLY" == *" $NAME "* ]] || continue
  [[ -f "$PROJECT/$MAP" ]] || { echo "build_movies: map $PROJECT/$MAP does not exist" >&2; exit 1; }
  SOUND="$HERE/$NAME.wav"
  [[ -f "$SOUND" ]] || { echo "build_movies: sound $SOUND does not exist" >&2; exit 1; }
  "$RUNTIME" --project "$PROJECT/Mirage.deproj" --render-movie "$MAP" --movie-out "$WORK/$NAME" \
             --resolution "$RES" --fps "$FPS" --duration "$DURATION"
  ffmpeg -hide_banner -loglevel error -y \
    -i "$SOUND" -t "$DURATION" \
    -c:a libopus -b:a 160k "$WORK/$NAME.opus"
  ffmpeg -hide_banner -loglevel error -y \
    -framerate "$FPS" -i "$WORK/$NAME/%05d.png" -i "$WORK/$NAME.opus" \
    -vf "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p10le" \
    -c:v libsvtav1 -preset 4 -crf 20 -pix_fmt yuv420p10le \
    -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv \
    -c:a copy -shortest "$OUT/$NAME.webm"
  echo "built $OUT/$NAME.webm"
done
