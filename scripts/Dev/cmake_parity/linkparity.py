"""CMAKE-4 link parity: premake vcxproj link settings vs CMake (file-API link/archive fragments), per target and config.

usage: python linkparity.py [report.json]   (report.json written by parity.py)
Prints one line per (target, config, field) mismatch, then a count. Semantic, not textual:
  type        Application/ConsoleApplication/StaticLibrary/DynamicLibrary  ==  EXECUTABLE/STATIC_LIBRARY/SHARED_LIBRARY
  subsystem   premake SubSystem (MSBuild default Console)                 ==  /subsystem:<x> fragment
  entry       /ENTRY:<x> in premake AdditionalOptions or EntryPointSymbol ==  /ENTRY:<x> fragment
  wholearch   /WHOLEARCHIVE:<lib> basenames
  debug       GenerateDebugInformation != false  ==  /DEBUG
  optref/icf  OptimizeReferences / EnableCOMDATFolding true  ==  /OPT:REF / /OPT:ICF
  incremental LinkIncremental (MSBuild default: true when UseDebugLibraries)  ==  /INCREMENTAL vs /INCREMENTAL:NO
  syslibs     AdditionalDependencies + MSBuild CoreLibraryDependencies  ==  bare .lib fragments (CMAKE_CXX_STANDARD_LIBRARIES)
  ourlibs     ProjectReference static libraries the exe links, closed over the static libraries' own references  ==  build/Bin/<cfg>/*.lib fragments
  name/outdir TargetName+TargetExt, OutDir  ==  nameOnDisk, artifact directory
"""
import json, os, sys

R = json.load(open(sys.argv[1] if len(sys.argv) > 1 else 'F:/aftmp/cmake3/report.json'))
CORE = {'kernel32.lib', 'user32.lib', 'gdi32.lib', 'winspool.lib', 'comdlg32.lib', 'advapi32.lib', 'shell32.lib',
        'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'odbc32.lib', 'odbccp32.lib'}
TYPES = {'Application': 'EXECUTABLE', 'StaticLibrary': 'STATIC_LIBRARY', 'DynamicLibrary': 'SHARED_LIBRARY',
         'Utility': 'UTILITY'}
static_projects = set()
for n, cs in R.items():
    for c, v in cs.items():
        if 'link' in v and v['link']['premake']['type'] == 'StaticLibrary':
            static_projects.add(n)


def closure(cfg, refs):
    # MSBuild: a static library's own ProjectReferences and Lib AdditionalDependencies reach the exe that links it
    # (lib.exe merges them; CMake passes them on the link line as the static library's link interface). Same symbols.
    seen, libs, todo = set(), set(), [r for r in refs if r in static_projects]
    while todo:
        r = todo.pop()
        if r in seen:
            continue
        seen.add(r)
        lk = R.get(r, {}).get(cfg, {}).get('link')
        if lk:
            libs |= set(lk['premake']['libs'])
            todo += [x for x in lk['premake']['refs'] if x in static_projects]
    return seen, libs

out = []
for n, cs in sorted(R.items()):
    for c, v in sorted(cs.items()):
        if 'link' not in v:
            continue
        p, k = v['link']['premake'], v['link']['cmake']
        fr = k['fragments']
        toks = [t for f in fr for t in f.split()]
        low = [t.lower() for t in toks]
        diff = {}
        if TYPES.get(p['type']) != k['type']:
            diff['type'] = (p['type'], k['type'])
        if p['targetname'].lower() != (k['nameOnDisk'] or '').lower():
            diff['name'] = (p['targetname'], k['nameOnDisk'])
        pdir = p['outdir'].rstrip('/')
        kdir = os.path.dirname(k['artifacts'][0]) if k['artifacts'] else None
        # A Utility project (BuildAllTests, RunAllTests) writes no binary; its OutDir is MSBuild bookkeeping only.
        if pdir != kdir and not (p['type'] == 'Utility' and k['type'] == 'UTILITY'):
            diff['outdir'] = (pdir, kdir)
        if k['type'] in ('EXECUTABLE', 'SHARED_LIBRARY'):
            sub = [t.split(':', 1)[1] for t in low if t.startswith('/subsystem:')]
            psub = (p['subsystem'] or 'Console').lower()
            if [psub] != sub:
                diff['subsystem'] = (psub, sub)
            pent = [o.split(':', 1)[1] for o in p['opts'] if o.upper().startswith('/ENTRY:')] + ([p['entry']] if p['entry'] else [])
            kent = [t.split(':', 1)[1] for t in toks if t.upper().startswith('/ENTRY:')]
            if pent != kent:
                diff['entry'] = (pent, kent)
            pwa = sorted(os.path.basename(o.split(':', 1)[1]).lower() for o in p['opts'] if o.upper().startswith('/WHOLEARCHIVE:'))
            kwa = sorted(os.path.basename(t.split(':', 1)[1]).lower() for t in toks if t.upper().startswith('/WHOLEARCHIVE:'))
            if pwa != kwa:
                diff['wholearch'] = (pwa, kwa)
            pdbg = p['debug'] != 'false'
            if pdbg != ('/debug' in low):
                diff['debug'] = (p['debug'], '/debug' in low)
            for key, flag in (('opt_ref', '/opt:ref'), ('icf', '/opt:icf')):
                if (p[key] == 'true') != (flag in low):
                    diff[key] = (p[key], flag in low)
            pinc = p['incremental'] if p['incremental'] is not None else ('true' if c == 'Debug' else 'false')
            kinc = 'true' if '/incremental' in low else 'false' if '/incremental:no' in low else ('true' if '/debug' in low else 'false')
            if pinc != kinc:
                diff['incremental'] = (p['incremental'], kinc)
            pclosure, plibs = closure(c, p['refs'])
            psys = set(p['libs']) | CORE | plibs
            ksys = set(os.path.basename(t).lower() for t in toks if t.lower().endswith('.lib') and not t.startswith('/')
                       and '\\build\\bin\\' not in t.lower() and '/build/bin/' not in t.lower())
            # MSBuild's CoreLibraryDependencies has odbc32/odbccp32, CMake's standard libraries do not: no code uses them.
            dsys = (sorted(psys - ksys - {'odbc32.lib', 'odbccp32.lib'}), sorted(ksys - psys))
            if dsys[0] or dsys[1]:
                diff['syslibs'] = dsys
            pours = set(R[r][c]['link']['premake']['targetname'].lower() for r in pclosure)
            kours = set(os.path.basename(t).lower() for t in toks if t.lower().endswith('.lib') and ('\\build\\bin\\' in t.lower() or '/build/bin/' in t.lower())
                        and not t.upper().startswith('/WHOLEARCHIVE:'))
            kours |= set(kwa)
            dours = (sorted(pours - kours), sorted(kours - pours))
            if dours[0] or dours[1]:
                diff['ourlibs'] = dours
        for f, d in diff.items():
            out.append((n, c, f, d))
for n, c, f, d in out:
    print(n, c, f, json.dumps(d)[:400])
print('mismatches:', len(out))
