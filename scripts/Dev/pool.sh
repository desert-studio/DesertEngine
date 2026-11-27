#!/usr/bin/env bash
# pool.sh <command> -- the machine's build pool (Tools/BuildPool/README.md) for scripts and people. The home is
# BUILDPOOL_HOME, else the one this tree was configured with (CMakeCache.txt BUILDPOOL_HOME).
#   bootstrap                            build the pool binary of this tree's revision into the home; print its path
#   start | status | window | stop       the home's daemon (status and window start one when none runs)
#   run [--class C] [--label L] -- cmd   run one build as a pool build: BUILDPOOL_CLASS (a [class.*] of pool.ini; absent
#                                        = inherited, else default_class), BUILDPOOL_RUN, CMAKE_BUILD_PARALLEL_LEVEL = `buildpool window`
#   attach [<tree>]                      put <tree>/build/cmake on the home's volume (junction / symlink); moved once
#   adopt-cache <dir>                    move an existing ccache directory into <home>/ccache, once
set -u
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh" || exit 1

die() { echo "pool.sh: $*" >&2; exit 1; }
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) POOL_WIN=1 ;; *) POOL_WIN=0 ;; esac
# A path as the native tools take it (Windows: C:/x, forward slashes).
native() { if [ "$POOL_WIN" = 1 ]; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

pool_home() {
    local h="${BUILDPOOL_HOME:-}"
    [ -n "$h" ] || h="$(sed -n 's/^BUILDPOOL_HOME:PATH=//p' "$DEV_BUILD/CMakeCache.txt" 2>/dev/null)"
    [ -n "$h" ] || die "BUILDPOOL_HOME is not set and $DEV_BUILD is not configured with a pool home"
    mkdir -p "$h" || die "cannot create the pool home $h"
    native "$(cd "$h" && pwd)"
}

# The binary of this tree's BuildPool revision; built into the home the first time (BuildPoolBootstrap.cmake).
pool_bin() {
    local out bin
    out="$(cmake -DBUILDPOOL_HOME="$HOME_DIR" -P "$DEV_ROOT/Tools/BuildPool/cmake/BuildPoolBootstrap.cmake" 2>&1)" ||
        die "bootstrap failed:
$out"
    bin="$(printf '%s\n' "$out" | sed -n 's/^buildpool: //p' | tail -1 | tr -d '\r')"
    [ -n "$bin" ] && [ -x "$bin" ] || die "bootstrap printed no binary:
$out"
    printf '%s\n' "$bin"
}

# A tree's volume: the drive letter on Windows, the device number elsewhere.
volume_of() {
    if [ "$POOL_WIN" = 1 ]; then cygpath -m "$1" | cut -c1 | tr '[:lower:]' '[:upper:]'
    else stat -f %d "$1" 2>/dev/null || stat -c %d "$1"; fi
}

# The directory a junction / symlink at $1 points to ("" when $1 is a plain directory or absent).
link_target() {
    if [ "$POOL_WIN" = 1 ]; then
        local parent name
        parent="$(cygpath -w "$(dirname "$1")")"; name="$(basename "$1")"
        # //AL: Git Bash rewrites a lone /AL into a path before cmd.exe sees it.
        cmd //c dir //AL "$parent" 2>/dev/null | tr -d '\r' |
            sed -n "s#.*<JUNCTION>[[:space:]]*$name \[\(.*\)\]\$#\1#p" | head -1 | { read -r t && cygpath -m "$t"; }
    elif [ -L "$1" ]; then
        readlink "$1"
    fi
}

cmd="${1:-}"; [ $# -gt 0 ] && shift
case "$cmd" in
    -h|--help|"") dev_help "$0"; exit 0 ;;
esac
HOME_DIR="$(pool_home)" || exit 1

case "$cmd" in
bootstrap)
    pool_bin
    ;;
start|status)
    BP="$(pool_bin)" || exit 1
    "$BP" status --home "$HOME_DIR"
    ;;
window|stop)
    BP="$(pool_bin)" || exit 1
    "$BP" "$cmd" --home "$HOME_DIR"
    ;;
run)
    class=""; label=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --class) class="${2:?--class needs a name}"; shift 2 ;;
            --label) label="${2:?--label needs a text}"; shift 2 ;;
            --) shift; break ;;
            *) die "run: unknown argument '$1' (run [--class C] [--label L] -- <command...>)" ;;
        esac
    done
    [ $# -gt 0 ] || die "run: no command after --"
    BP="$(pool_bin)" || exit 1
    window="$("$BP" window --home "$HOME_DIR" | tr -d '\r')" || die "run: buildpool window failed"
    case "$window" in ''|*[!0-9]*) die "run: buildpool window answered '$window', not a number" ;; esac
    # The home goes with the build, so a configure the command runs takes the same pool.
    export BUILDPOOL_HOME="$HOME_DIR" CMAKE_BUILD_PARALLEL_LEVEL="$window"
    # Without --class an inherited BUILDPOOL_CLASS stays (BUILDPOOL_CLASS=lead bash compile_check.sh).
    [ -n "$class" ] && export BUILDPOOL_CLASS="$class"
    if [ -n "$label" ]; then export BUILDPOOL_RUN="$label"; else unset BUILDPOOL_RUN; fi
    exec "$@"
    ;;
attach)
    tree="${1:-$DEV_ROOT}"
    [ -d "$tree" ] || die "attach: no tree $tree"
    tree="$(native "$(cd "$tree" && pwd)")"
    src="$tree/build/cmake"
    if [ "$(volume_of "$tree")" = "$(volume_of "$HOME_DIR")" ]; then
        echo "pool.sh: $tree is on the home's volume already; $src stays where it is"; exit 0
    fi
    id="$(basename "$tree")-$(printf '%s' "$tree" | tr '[:upper:]' '[:lower:]' | { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-8)"
    dst="$HOME_DIR/trees/$id/cmake"
    current="$(link_target "$src")"; moved=""
    if [ -n "$current" ]; then
        [ "$(native "$current")" = "$dst" ] && { echo "pool.sh: $src -> $dst (attached already)"; exit 0; }
        die "attach: $src already points to $current, not $dst"
    fi
    mkdir -p "$(dirname "$dst")" || die "attach: cannot create $(dirname "$dst")"
    if [ -d "$src" ]; then
        [ -e "$dst" ] && die "attach: both $src and $dst exist; remove the one that is stale"
        echo "pool.sh: moving $src to $dst (once; the tree must not be building)"
        mv "$src" "$dst" || die "attach: moving $src to $dst failed"
        moved=1
    else
        mkdir -p "$dst" "$(dirname "$src")" || die "attach: cannot create $dst"
    fi
    if [ "$POOL_WIN" = 1 ]; then
        # //J: Git Bash rewrites a lone /J into a path before cmd.exe sees it.
        if ! cmd //c mklink //J "$(cygpath -w "$src")" "$(cygpath -w "$dst")" >/dev/null; then
            [ -n "$moved" ] && { mv "$dst" "$src" || die "attach: mklink /J $src failed, and moving $dst back failed"; }
            die "attach: mklink /J $src failed"
        fi
    else
        ln -s "$dst" "$src" || die "attach: ln -s $dst $src failed"
    fi
    echo "pool.sh: $src -> $dst"
    ;;
adopt-cache)
    from="${1:?adopt-cache needs the old cache directory}"
    [ -d "$from" ] || die "adopt-cache: no directory $from"
    to="$HOME_DIR/ccache"
    mkdir -p "$to" || die "adopt-cache: cannot create $to"
    [ -z "$(ls -A "$to" | grep -v -x -e tmp -e ccache.conf)" ] || die "adopt-cache: $to holds a cache already"
    # Results and manifests only: the old ccache.conf and tmp/ stay behind -- the build's settings are BuildPool.cmake's.
    moved=0
    for entry in "$from"/*; do
        [ -e "$entry" ] || continue
        case "$(basename "$entry")" in tmp|ccache.conf) continue ;; esac
        mv "$entry" "$to/" || die "adopt-cache: moving $entry failed"
        moved=$((moved + 1))
    done
    [ "$moved" -gt 0 ] || die "adopt-cache: nothing to adopt in $from"
    echo "pool.sh: $moved entries of $from moved to $to; $from/ccache.conf no longer decides anything"
    ;;
*)
    die "unknown command '$cmd' (bootstrap, start, status, window, stop, run, attach, adopt-cache)"
    ;;
esac
