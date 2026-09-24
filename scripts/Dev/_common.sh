#!/usr/bin/env bash
# Shared by scripts/Dev/*.sh: the tree root, a per-run log directory, and a per-test time cap.
# Logs live under build/DevLogs (ignored by git, one copy per worktree) so parallel agents never share them.

DEV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# dev_logdir <name> — creates and prints build/DevLogs/<name>-<timestamp>
dev_logdir() {
    local d="$DEV_ROOT/build/DevLogs/$1-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$d" && echo "$d"
}

# dev_capped <seconds> <cmd...> — runs cmd; after <seconds> SIGKILLs it and returns 124 (macOS has no `timeout`).
# A watchdog parent, not `alarm; exec`: a test that blocks or handles SIGALRM outlived that cap by 40 minutes.
dev_capped() {
    perl -e '$t = shift; $p = fork; if (!$p) { exec @ARGV; exit 127 }
             $SIG{ALRM} = sub { kill 9, $p; waitpid $p, 0; exit 124 }; alarm $t; waitpid $p, 0;
             exit( ($? & 127) ? 128 + ($? & 127) : $? >> 8 )' "$@"
}

# dev_help <file> — prints the script's leading comment block (the 5-line --help).
dev_help() {
    sed -n '2,/^[^#]/p' "$1" | sed -n '/^#/s/^# \{0,1\}//p'
}
