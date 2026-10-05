#!/usr/bin/env bash
# Builds the three startup movies with ONE command: the engine renders each movie map (Runtime
# --render-movie: offscreen 3840x2160, fixed 1/60 s step), the sound accent is synthesised here from the
# parameters below, and ffmpeg encodes AV1 (SVT-AV1, 10-bit 4:2:0, Rec.709 tv range) + Opus into WebM.
# Usage: build_movies.sh <Runtime binary> [movie name...]   (no names = all three)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$(cd "$HERE/.." && pwd)"; PROJECT="$(cd "$HERE/../../.." && pwd)"
RUNTIME="${1:?usage: build_movies.sh <Runtime binary> [movie name...]}"; shift
ONLY=" $* "
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
FPS=60; RES=3840x2160

# name | map (relative to the project) | seconds | sound accent (ffmpeg aevalsrc expression, stereo)
MOVIES=(
  "DesertEngine|Content/Movies/Source/DesertEngine.desce|3.0|0.30*sin(2*PI*(110+40*t)*t)*exp(-1.6*t)+0.12*sin(2*PI*220*t)*exp(-2.4*t)"
  "DesertStudio|Content/Movies/Source/DesertStudio.desce|3.0|0.22*(sin(2*PI*196*t)+sin(2*PI*293.66*t)+0.6*sin(2*PI*392*t))*(1-exp(-3*t))*exp(-0.9*t)"
  "Title|Content/Movies/Source/Title.desce|4.0|0.25*sin(2*PI*73.42*t+2*sin(2*PI*0.5*t))*(1-exp(-2*t))*exp(-0.5*t)+0.05*(random(0)-0.5)*exp(-1.5*t)"
)

for entry in "${MOVIES[@]}"; do
  # NOT "SECONDS": that is bash's clock (an assigned "3.0" reads back as "0", an assigned "3" as 3 plus the
  # seconds elapsed since, so the sound length after a render was wrong too).
  IFS='|' read -r NAME MAP DURATION SOUND <<<"$entry"
  [[ "$ONLY" == "  " || "$ONLY" == *" $NAME "* ]] || continue
  [[ -f "$PROJECT/$MAP" ]] || { echo "build_movies: map $PROJECT/$MAP does not exist" >&2; exit 1; }
  "$RUNTIME" --project "$PROJECT/Mirage.deproj" --render-movie "$MAP" --movie-out "$WORK/$NAME" \
             --resolution "$RES" --fps "$FPS" --duration "$DURATION"
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "aevalsrc=exprs='$SOUND|$SOUND':s=48000:d=$DURATION" \
    -af "afade=t=out:st=$(echo "$DURATION - 0.6" | bc):d=0.6,alimiter=limit=0.9" \
    -c:a libopus -b:a 160k "$WORK/$NAME.opus"
  ffmpeg -hide_banner -loglevel error -y \
    -framerate "$FPS" -i "$WORK/$NAME/%05d.png" -i "$WORK/$NAME.opus" \
    -vf "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p10le" \
    -c:v libsvtav1 -preset 4 -crf 20 -pix_fmt yuv420p10le \
    -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv \
    -c:a copy -shortest "$OUT/$NAME.webm"
  echo "built $OUT/$NAME.webm"
done
