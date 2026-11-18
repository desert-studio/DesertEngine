#!/bin/bash
# header_fanout.sh [N] [--all] [--objects <header>] [--gate] — what an edit of a header costs: how many objects the
# last build compiled with it (Ninja's own dependency log, plus every object built on a precompiled header that holds
# it), all objects and the Editor's; the top N (default 30) of the tree's headers (ThirdParty with --all).
# --objects <header>: the objects one header rebuilds. --gate: every header of scripts/Dev/header_fanout_budget.txt
# within its ceiling (exit 1 otherwise) -- a split header that starts pulling the world back in fails here, not in a
# 20-minute rebuild. Reads the Debug build of this tree: build first (a never-built object has no dependency record).
set -u
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
case "${1:-}" in -h|--help) dev_help "$0"; exit 0 ;; esac
TOP=30; ALL=0; OBJECTS=""; GATE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --all) ALL=1 ;;
        --objects) OBJECTS="${2:?--objects <header>}"; shift ;;
        --gate) GATE=1 ;;
        [0-9]*) TOP="$1" ;;
        *) echo "header_fanout: unknown argument '$1' (--help)"; exit 2 ;;
    esac
    shift
done
[ -f "$DEV_BUILD/build-Debug.ninja" ] || { echo "header_fanout: no Debug build in $DEV_BUILD -- build Editor first"; exit 1; }
LOG="$(dev_logdir header_fanout)"
cd "$DEV_BUILD" || exit 1
ninja -f build-Debug.ninja -t deps > "$LOG/deps.txt" 2> "$LOG/deps.err" || { echo "header_fanout: ninja -t deps failed: $(head -1 "$LOG/deps.err")"; exit 1; }
ninja -f build-Debug.ninja -t inputs Editor 2>/dev/null | grep -E '\.(o|obj)$' > "$LOG/editor_objects.txt"

python3 - "$DEV_ROOT" "$DEV_BUILD" "$LOG" "$TOP" "$ALL" "$OBJECTS" "$GATE" <<'EOF'
import os, sys
root, build, log, top, show_all, one, gate = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5] == '1', sys.argv[6], sys.argv[7] == '1'
HDR = ('.h', '.hpp', '.hh', '.hxx', '.inl', '.ipp', '.inc')

def rel(p):  # a dependency path -> relative to the tree root, or None for the SDK / toolchain / Homebrew
    a = os.path.normpath(p if os.path.isabs(p) else os.path.join(build, p))
    return os.path.relpath(a, root) if a.startswith(root + os.sep) else None

deps, cur = {}, None
for line in open(os.path.join(log, 'deps.txt')):
    if not line.startswith(' '):
        cur = line.split(':', 1)[0] if '#deps' in line else None
        if cur: deps[cur] = []
    elif cur and line.strip():
        deps[cur].append(line.strip())

# A precompiled header's own headers reach every object that is compiled with it: such an object names the PCH's
# header (<dir>/cmake_pch.hxx) among its dependencies; the PCH's record is <dir>/cmake_pch.hxx.pch (.gch with gcc).
pch = {o.rsplit('.', 1)[0]: d for o, d in deps.items() if o.endswith(('.pch', '.gch'))}
editor = {l.strip() for l in open(os.path.join(log, 'editor_objects.txt')) if l.strip()}
count, ecount, users = {}, {}, {}
objects = [o for o in deps if o.endswith(('.o', '.obj'))]
for o in objects:
    hs = set()
    for d in deps[o]:
        hs.add(d)
        if d in pch: hs.update(pch[d])
    for d in hs:
        r = rel(d)
        if not r or not r.endswith(HDR) or r.startswith('build' + os.sep): continue
        count[r] = count.get(r, 0) + 1
        if o in editor: ecount[r] = ecount.get(r, 0) + 1
        if one and r == one: users.setdefault(r, []).append(o)

if one:
    hit = sorted(users.get(one, []))
    print(f'{one}: {len(hit)} object(s), {sum(1 for o in hit if o in editor)} of them the Editor\'s')
    for o in hit: print('  ' + o)
    sys.exit(0 if one in count else 1)

if gate:
    budget = os.path.join(root, 'scripts', 'Dev', 'header_fanout_budget.txt')
    bad = 0
    for line in open(budget):
        line = line.split('#', 1)[0].split()
        if len(line) != 2: continue
        h, ceiling = line[0], int(line[1])
        if not os.path.exists(os.path.join(root, h)):
            print(f'GATE: {h} is in the budget but not in the tree'); bad += 1; continue
        n = count.get(h, 0)
        if n > ceiling: print(f'GATE: {h} reaches {n} objects, ceiling {ceiling}'); bad += 1
    print(f'header_fanout --gate: {"OK" if not bad else f"{bad} header(s) over budget"} ({len(objects)} objects)')
    sys.exit(1 if bad else 0)

rows = sorted(((n, h) for h, n in count.items() if show_all or not h.startswith('ThirdParty' + os.sep)), reverse=True)
print(f'{"objects":>7} {"Editor":>6}  header   ({len(objects)} objects, {len(editor)} of them the Editor\'s; log {log})')
for n, h in rows[:top]:
    print(f'{n:>7} {ecount.get(h, 0):>6}  {h}')
EOF
