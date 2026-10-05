#!/usr/bin/env bash
# Shared by scripts/Dev/*.sh: the tree root, a per-run log directory, and a per-test time cap.
# Logs live under build/DevLogs (ignored by git, one copy per worktree) so parallel agents never share them.

DEV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# premake writes the workspace Makefile and every <Project>.make here (BuildScripts/Workspace.lua `location`), never
# at the root; paths inside them are relative to this directory, so make always runs with `-C "$DEV_PROJECTS"`.
DEV_PROJECTS="$DEV_ROOT/build/Projects"

# dev_logdir <name> — creates and prints build/DevLogs/<name>-<timestamp>
dev_logdir() {
    local d="$DEV_ROOT/build/DevLogs/$1-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$d" && echo "$d"
}

# dev_capped <seconds> <cmd...> — runs cmd; after <seconds> SIGKILLs it and returns 124 (macOS has no `timeout`).
# A watchdog parent, not `alarm; exec`: a test that blocks or handles SIGALRM outlived that cap by 40 minutes.
#
# The Homebrew validation layer's manifest names its library bare (libVkLayer_khronos_validation.dylib), and dyld's
# default fallback on Apple Silicon is /usr/lib only, so every Vulkan suite got VK_ERROR_LAYER_NOT_PRESENT at
# vkCreateInstance (RDG-MAC1). DYLD_* cannot be exported from here: SIP strips it on the way through /usr/bin/env
# and /usr/bin/perl, so perl sets it in the child itself, before exec.
DEV_LAYER_DIR=""
[ "$(uname)" = Darwin ] && [ -f /opt/homebrew/lib/libVkLayer_khronos_validation.dylib ] && DEV_LAYER_DIR=/opt/homebrew/lib
export DEV_LAYER_DIR
dev_capped() {
    perl -e '$t = shift; $p = fork;
             if (!$p) { if ($ENV{DEV_LAYER_DIR}) { $ENV{DYLD_FALLBACK_LIBRARY_PATH} = $ENV{DEV_LAYER_DIR} } exec @ARGV; exit 127 }
             $SIG{ALRM} = sub { kill 9, $p; waitpid $p, 0; exit 124 }; alarm $t; waitpid $p, 0;
             exit( ($? & 127) ? 128 + ($? & 127) : $? >> 8 )' "$@"
}

# dev_help <file> — prints the script's leading comment block (the 5-line --help).
dev_help() {
    sed -n '2,/^[^#]/p' "$1" | sed -n '/^#/s/^# \{0,1\}//p'
}

# A merge can add sources to a project without touching its premake5.lua (files are globbed): after the AF7 merge
# Common.make listed none of TextAssetHeader.cpp and every link failed. Regenerate the makefiles when a premake
# script or the set of tracked sources changed since they were written. $1 = log dir.
dev_regen_makefiles() {
    local sums; sums=$(git -C "$DEV_ROOT" ls-files '*.cpp' '*.mm' '*.c' 'premake5.lua' '*/premake5.lua' | md5)
    [ "$sums" = "$(cat "$DEV_ROOT/build/DevLogs/.sources.md5" 2>/dev/null)" ] && \
        [ -z "$(find "$DEV_ROOT" -maxdepth 4 -name premake5.lua -newer "$DEV_PROJECTS/Common.make" 2>/dev/null | head -1)" ] && return 0
    (cd "$DEV_ROOT" && CI=true premake5 gmake) >"$1/premake.log" 2>&1 || { echo "premake5 gmake FAILED; log $1/premake.log"; return 1; }
    mkdir -p "$DEV_ROOT/build/DevLogs" && echo "$sums" >"$DEV_ROOT/build/DevLogs/.sources.md5"
}
