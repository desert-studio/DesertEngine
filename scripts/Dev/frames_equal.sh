#!/usr/bin/env bash
# frames_equal.sh [--frames "3 90"] <A> <B> [bench-id...] — capture each bench view with A and B, compare byte for byte.
#   A, B: an editor binary (<tree>/build/Bin/Debug/Editor[.exe], run in its own tree) or a baseline directory holding
#   png/<id>_f<N>_a.png (frames_equal.sh --capture). `frames_equal.sh E E` is the determinism of one build.
# frames_equal.sh --capture <exe> <out-dir> [--frames "3 90"] [--tag a] [bench-id...] [-- extra editor args]
# Bench ids (default: all): see bench() below. ~1-2 min per capture on the Windows bench (Debug); all 7 x 2 x 2 ~ 45 min.
# On a difference: share of pixels, max delta, bbox and a diff PNG. Exit 0 = all equal, 1 = a difference, 2 = error.
set -u
source "$(dirname "$0")/_common.sh"
[ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] || [ $# -eq 0 ] && { dev_help "$0"; exit 0; }

# THE BENCH: <id> -> scene file, camera and mode, fixed here and nowhere else (plan RDG0 section 3.4). A comparison is
# only as sensitive as the frame is informative: the first measurement looked from 2 m almost straight up at the sky.
DEFAULT_BENCH="Starter DeferredSSRGI Clouds_Zenith Clouds_Mid Clouds_Horizon LandscapeParticles UI_ListProbe"
bench() {  # bench <id> -> "<scene> <editor args...>"
    case "$1" in
        Starter)            echo "Starter --camera 0,200,400 --look 0,-0.3,-1" ;;
        DeferredSSRGI)      echo "RDG_DeferredSSRGI --camera 0,250,700 --look 0,-0.3,-1" ;;    # deferred, SSR + GI on
        Clouds_Zenith)      echo "Clouds_Protocol --camera 0,200,0 --look 0,1,0.01" ;;
        Clouds_Mid)         echo "Clouds_Protocol --camera 0,200,0 --look 0,1,-1" ;;
        Clouds_Horizon)     echo "Clouds_Protocol --camera 0,200,0 --look 0,0,-1" ;;
        LandscapeParticles) echo "RDG_LandscapeParticles --camera 0,400,1800 --look 0,-0.2,-1 --play" ;;
        UI_ListProbe)       echo "UI_ListProbe --camera 0,250,700 --look 0,-0.3,-1" ;;      # grid + screen-space canvas
        Clouds_HeroTrio)    echo "Clouds_HeroTrio --camera 0,200,0 --look 0,0.12,-1" ;;      # not in the default bench
        *) echo "frames_equal: unknown bench id '$1' (known: $DEFAULT_BENCH)" >&2; return 1 ;;
    esac
}

case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) WIN=1; PY=python ;; *) WIN=0; PY=python3 ;; esac
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi; }
nativepath() { if [ $WIN = 1 ]; then cygpath -m "$1"; else echo "$1"; fi; }
editor_running() {
    if [ $WIN = 1 ]; then tasklist //FI "IMAGENAME eq Editor.exe" 2>/dev/null | grep -qi '^Editor.exe'; else pgrep -x Editor >/dev/null; fi
}

# capture <exe> <bench-id> <frames> <out.png> <logdir> [extra...] — one editor run in a fresh HOME; 0 = PNG written.
# The editor's exit code alone does not decide: 3 is CrashHandler's (Common/Core/CrashHandler.cpp), raised by a crash
# in teardown AFTER the capture was written; such a run counts as captured only when engine_log.txt says the PNG was
# written and the file is newer than the start, and it is reported, never folded into success silently.
capture() {
    local exe=$1 id=$2 frames=$3 out=$4 logs=$5; shift 5
    local spec; spec=$(bench "$id") || return 2
    local scene=${spec%% *} cam=${spec#* }
    local tree; tree="$(cd "$(dirname "$exe")/../../.." && pwd)"
    local sc="$tree/Editor/Resources/Assets/Scenes/$scene.desce"
    [ -f "$sc" ] || { echo "no scene $sc" >&2; return 2; }
    local runner="$tree/.claude/tools/run_capped.py"
    [ -f "$runner" ] || { echo "no $runner (the memory-capped launcher)" >&2; return 2; }
    local tag; tag=$(basename "$out" .png)
    local home="$logs/home-$tag"; rm -rf "$home"; mkdir -p "$home/AppData/Roaming" "$home/AppData/Local"
    for i in $(seq 180); do editor_running || break; [ "$i" = 1 ] && echo "waiting for another Editor to exit" >&2; sleep 5; done
    editor_running && { echo "another Editor is still running after 15 min" >&2; return 2; }
    # A crash marker left by an earlier run turns the whole window into the "Recover unsaved work?" dialog.
    rm -f "$tree/Editor/Resources/Assets/Scenes/Autosave/.session.lock" "$out"
    local t0; t0=$(date +%s)
    # shellcheck disable=SC2086  # $cam is a word list on purpose
    ( cd "$tree/Editor" && HOME="$(nativepath "$home")" USERPROFILE="$(nativepath "$home")" \
        APPDATA="$(nativepath "$home/AppData/Roaming")" LOCALAPPDATA="$(nativepath "$home/AppData/Local")" \
        dev_capped 900 "$PY" "$runner" "$exe" --project Desert.deproj --scene "Resources/Assets/Scenes/$scene.desce" \
        --shot "$(nativepath "$out")" --shot-frames "$frames" $cam "$@" >"$logs/$tag.out" 2>&1 )
    local rc=$?
    cp "$tree/Editor/engine_log.txt" "$logs/$tag.engine_log.txt" 2>/dev/null
    printf "%s\t%s\t%s\t%s\n" "$tag" "$rc" "$(($(date +%s) - t0))" "$exe --scene $scene --shot-frames $frames $cam $*" >>"$logs/runs.tsv"
    [ -f "$out" ] && [ "$(date -r "$out" +%s)" -ge "$t0" ] || { echo "$scene: no PNG (exit $rc; $logs/$tag.out)" >&2; return 2; }
    [ $rc = 0 ] && return 0
    if [ $rc = 3 ] && grep -q "\[Shot\] wrote" "$logs/$tag.engine_log.txt" 2>/dev/null; then
        echo "$tag" >>"$logs/teardown_crash.txt"
        return 0
    fi
    echo "$scene: exit $rc ($logs/$tag.out)" >&2
    return 2
}

frames_list="3 90"
if [ "$1" = "--capture" ]; then
    shift; exe=$1; outdir=$2; shift 2; tag=a; ids=(); extra=()
    while [ $# -gt 0 ]; do
        case "$1" in --frames) frames_list=$2; shift 2 ;; --tag) tag=$2; shift 2 ;; --) shift; extra=("$@"); break ;; *) ids+=("$1"); shift ;; esac
    done
    [ ${#ids[@]} -eq 0 ] && read -ra ids <<<"$DEFAULT_BENCH"
    mkdir -p "$outdir/png" "$outdir/logs"; outdir="$(cd "$outdir" && pwd)"
    [ -f "$outdir/logs/runs.tsv" ] || printf "tag\texit\tseconds\tcommand\n" >"$outdir/logs/runs.tsv"
    rc=0
    for id in "${ids[@]}"; do for n in $frames_list; do
        capture "$exe" "$id" "$n" "$outdir/png/${id}_f${n}_$tag.png" "$outdir/logs" ${extra[@]+"${extra[@]}"} || rc=2
    done; done
    exit $rc
fi

[ "$1" = "--frames" ] && { frames_list=$2; shift 2; }
[ $# -ge 2 ] || { dev_help "$0"; exit 2; }
A=$1; B=$2; shift 2
ids=("$@"); [ ${#ids[@]} -eq 0 ] && read -ra ids <<<"$DEFAULT_BENCH"
for id in "${ids[@]}"; do bench "$id" >/dev/null || exit 2; done
logs=$(dev_logdir frames_equal); mkdir -p "$logs/png"
printf "tag\texit\tseconds\tcommand\n" >"$logs/runs.tsv"
# side <A|B> <exe-or-dir> <id> <frames> -> prints the PNG of that side (captured now, or the baseline's)
side() {
    if [ -d "$2" ]; then
        local p="$2/png/${3}_f${4}_a.png"; [ -f "$p" ] || { echo "baseline has no $p" >&2; return 2; }; echo "$p"
    else
        capture "$2" "$3" "$4" "$logs/png/${3}_f${4}_$1.png" "$logs" && echo "$logs/png/${3}_f${4}_$1.png"
    fi
}
equal=0; differ=0; failed=0; lines=()
for id in "${ids[@]}"; do for n in $frames_list; do
    k="${id}_f$n"
    if ! pa=$(side A "$A" "$id" "$n") || ! pb=$(side B "$B" "$id" "$n"); then
        lines+=("$k: CAPTURE FAILED (see $logs/runs.tsv)"); failed=$((failed + 1)); continue
    fi
    ha=$(sha "$pa"); hb=$(sha "$pb")
    if [ "$ha" = "$hb" ]; then
        equal=$((equal + 1)); echo "$k: equal ${ha:0:12}" >>"$logs/summary.txt"
    else
        detail=$("$PY" "$DEV_ROOT/scripts/Dev/frames_compare.py" "$pa" "$pb" "$logs/png/${k}_diff.png")
        lines+=("$k: DIFFER, $detail; $logs/png/${k}_diff.png"); differ=$((differ + 1))
    fi
done; done
# Equal views are one count, differences one line each: the verdict stays within ten lines for the whole bench.
[ ${#lines[@]} -gt 0 ] && printf '%s\n' "${lines[@]}" | tee -a "$logs/summary.txt" | head -8
[ -f "$logs/teardown_crash.txt" ] && echo "note: $(wc -l <"$logs/teardown_crash.txt" | tr -d ' ') run(s) exited 3 = crash in teardown AFTER the PNG was written ($logs/teardown_crash.txt)"
echo "frames_equal: $equal equal, $differ differ, $failed failed (frames: $frames_list); $logs/summary.txt"
[ $failed -gt 0 ] && exit 2
[ $differ -gt 0 ] && exit 1
exit 0
