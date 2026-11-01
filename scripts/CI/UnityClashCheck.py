#!/usr/bin/env python3
"""Approximate the Windows unity batches with clang -fsyntax-only on macOS.
Usage: UnityClashCheck.py <repo root> <scratch dir> <N> [<Target>...]   (no target: every target CMake builds as unity,
i.e. Common, the engine module libraries Desert*, and the editor libraries Editor*)
after `scripts/MacOS/BuildMacOS.sh Debug --unity --gen-only` (preset macos-clang-ninja-unity), which writes the
CMake unity files and build/cmake/macos-clang-ninja-unity/compile_commands.json.
N is the batch size (cmake/DesertWorkspace.cmake, UNITY_BUILD_BATCH_SIZE); the opt-out list there decides which
sources CMake leaves out of the batches, and this script reads the batches CMake wrote, so it never re-derives it.
For each target: its sources in unity order (the #include lines of CMakeFiles/<Target>.dir/Unity/unity_<i>_cxx.cxx,
by <i>), regrouped into N contiguous sources at two offsets (0 and N/2), so every adjacent pair is covered once more
and a Windows-only source shifting the Windows batches by a few does not hide a clash. Each group is compiled with
the flags of that target's own unity entry in compile_commands.json (Debug). "0 with errors" is the pass.
Windows-only sources are not in the macOS batches at all; the Windows unity build itself covers them."""
import re, sys, json, shlex, subprocess, os, concurrent.futures as cf
ROOT, OUT, N = sys.argv[1], sys.argv[2], int(sys.argv[3])
targets = sys.argv[4:]
BUILD = os.path.join(ROOT, 'build', 'cmake', 'macos-clang-ninja-unity')
DB = os.path.join(BUILD, 'compile_commands.json')
if not os.path.exists(DB):
    sys.exit(f'{DB} does not exist: run scripts/MacOS/BuildMacOS.sh Debug --unity --gen-only first')
UNITY = re.compile(r'/CMakeFiles/([^/]+)\.dir/(?:[^/]+/)?Unity/unity_(\d+)_cxx\.cxx$')
INCLUDE = re.compile(r'^#include "([^"]+\.cpp)"$', re.M)
LAUNCHERS = {'ccache', 'sccache'}

def syntax_only(command, unity_file):
    """The unity entry's compile line with the object, the dependency file and the source taken out."""
    args = shlex.split(command)
    if os.path.basename(args[0]) in LAUNCHERS:
        args = args[1:]
    out, skip = [], 0
    for a in args:
        if skip:
            skip -= 1
            continue
        if a in ('-o', '-MF', '-MT', '-MQ'):
            skip = 1
            continue
        if a in ('-c', '-MD', '-MMD', '--') or a == unity_file:
            continue
        out.append(a)
    return out + ['-fsyntax-only', '-ferror-limit=0', '-w']

entries = {}
for e in json.load(open(DB)):
    m = UNITY.search(e['file'].replace('\\', '/'))
    if not m or (targets and m.group(1) not in targets):
        continue
    if '/Debug/' not in e.get('output', '/Debug/').replace('\\', '/'):
        continue
    entries.setdefault(m.group(1), []).append((int(m.group(2)), e))

jobs = []
for tgt in (targets or sorted(entries)):
    units = sorted(entries.get(tgt, []), key=lambda x: x[0])
    if not units:
        sys.exit(f'{tgt}: no unity entry in {DB} (not a unity target, or the name is not a CMake target)')
    order = []
    for _, e in units:
        order += INCLUDE.findall(open(e['file']).read())
    base = syntax_only(units[0][1]['command'], units[0][1]['file'])
    directory = units[0][1]['directory']
    print(f'{tgt}: {len(order)} unity sources in {len(units)} CMake batches')
    for off in (0, N // 2):
        groups = [order[:off]] if off else []
        groups += [order[i:i + N] for i in range(off, len(order), N)]
        for gi, g in enumerate(groups):
            if len(g) < 2:
                continue
            name = f'{OUT}/{tgt}_o{off}_{gi:03d}.cpp'
            open(name, 'w').write(''.join(f'#include "{s}"\n' for s in g))
            jobs.append((tgt, name, g, base + [name], directory))

def run(j):
    r = subprocess.run(j[3], cwd=j[4], capture_output=True, text=True)
    return j, r.returncode, r.stderr

res = []
with cf.ThreadPoolExecutor(3) as ex:
    for j, rc, err in ex.map(run, jobs):
        errs = [l for l in err.splitlines() if ' error: ' in l or 'fatal error' in l]
        if rc or errs:
            res.append((j[1], errs))
print(f'{len(jobs)} groups, {len(res)} with errors')
for name, errs in res:
    print('==', name)
    for e in sorted(set(errs))[:40]:
        print('  ', e.replace(ROOT + '/', ''))
