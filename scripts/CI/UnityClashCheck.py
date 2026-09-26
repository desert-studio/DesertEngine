#!/usr/bin/env python3
"""Approximate MSBuild unity groups with clang -fsyntax-only.
Usage: UnityClashCheck.py <repo root> <scratch dir> 12 Common Desert Editor, after `CI=true premake5 gmake`
and `CI=true VULKAN_SDK=/opt/homebrew PROGRAMFILES=/tmp premake5 vs2022 --os=windows --unity` in the root.
Re-derives the opt-out list in BuildScripts/UnityBuild.lua; "0 with errors" is the pass.
For each project: sources in .vcxproj item order, minus IncludeInUnityFile=false, pch Create, and
sources the macOS makefile does not compile (Windows-only). Groups of N contiguous sources, at two
offsets (0 and N/2) so every adjacent pair is covered once more. Prints clash-shaped errors."""
import re, sys, subprocess, os, concurrent.futures as cf
ROOT, OUT, N = sys.argv[1], sys.argv[2], int(sys.argv[3])
projects = sys.argv[4:]
def mkvars(mk):
    t = open(os.path.join(ROOT, mk)).read()
    inc = re.search(r'^INCLUDES \+= (.*)$', t, re.M).group(1)
    fi = re.search(r'^FORCE_INCLUDE \+=(.*)$', t, re.M).group(1)
    dbg = t[t.index('ifeq ($(config),debug)'):]
    de = re.search(r'^DEFINES \+= (.*)$', dbg, re.M).group(1)
    srcs = set(m.group(1) for m in re.finditer(r'^\$\(OBJDIR\)/\S+\.o: (\S+\.cpp)$', t, re.M))
    return inc, fi, de, srcs
jobs = []
for prj in projects:
    inc, fi, de, macsrcs = mkvars(prj + '.make')
    x = open(os.path.join(ROOT, prj + '.vcxproj')).read()
    items = re.findall(r'<ClCompile Include="([^"]+\.cpp)"(\s*/>|>(.*?)</ClCompile>)', x, re.S)
    order, skipped = [], []
    for path, _, body in items:
        p = path.replace('\\', '/')
        if 'IncludeInUnityFile>false' in (body or '') or 'PrecompiledHeader>Create' in (body or ''):
            continue
        (order if p in macsrcs else skipped).append(p)
    print(f'{prj}: {len(order)} unity sources checked, {len(skipped)} not compiled on macOS: {skipped}')
    for off in (0, N // 2):
        groups = [order[:off]] if off else []
        groups += [order[i:i + N] for i in range(off, len(order), N)]
        for gi, g in enumerate(groups):
            if len(g) < 2:
                continue
            name = f'{OUT}/{prj}_o{off}_{gi:03d}.cpp'
            open(name, 'w').write(''.join(f'#include "{ROOT}/{s}"\n' for s in g))
            cmd = (f'cd "{ROOT}" && clang++ -fsyntax-only -ferror-limit=0 -std=c++20 -arch arm64 -w '
                   f'{de} {inc} {fi} "{name}"')
            jobs.append((prj, name, g, cmd))
def run(j):
    r = subprocess.run(j[3], shell=True, capture_output=True, text=True)
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
