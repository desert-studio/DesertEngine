"""CMAKE-3 parity: premake vcxproj (as MSBuild expands it) vs CMake (compile_commands + file-API codemodel).

usage: python parity.py <worktree> <cmake-build-prefix> <out.json>
cmake build dirs are <prefix>-Debug, <prefix>-Release, <prefix>-Shipping.
"""
import glob, json, os, re, shlex, sys, collections
from xml.etree import ElementTree as ET

NS = '{http://schemas.microsoft.com/developer/msbuild/2003}'
WT = os.path.normpath(sys.argv[1])
PREFIX = sys.argv[2]
OUT = sys.argv[3]
CONFIGS = ['Debug', 'Release', 'Shipping']


def norm(p, base=None):
    p = p.strip().strip('"')
    if base and not os.path.isabs(p):
        p = os.path.join(base, p)
    return os.path.normcase(os.path.normpath(p)).replace('\\', '/')


def split_list(s):
    return [x for x in (s or '').split(';') if x and not x.startswith('%(') and not x.startswith('$(')]


def cond_cfg(cond):
    m = re.search(r"=='(\w+)\|x64'", cond or '')
    return m.group(1) if m else None


# ---------------------------------------------------------------- premake side
def premake_projects():
    out = {}
    for path in glob.glob(os.path.join(WT, 'build/Projects/**/*.vcxproj'), recursive=True):
        base = os.path.dirname(path)
        root = ET.parse(path).getroot()
        name = os.path.splitext(os.path.basename(path))[0]
        cfgs = {}
        for pc in root.iter(NS + 'ProjectConfiguration'):
            if pc.find(NS + 'Platform').text == 'x64':
                cfgs[pc.find(NS + 'Configuration').text] = {'props': {}, 'idg': {}}
        for pg in root.iter(NS + 'PropertyGroup'):
            c = cond_cfg(pg.get('Condition'))
            if c in cfgs:
                for e in pg:
                    cfgs[c]['props'][e.tag[len(NS):]] = e.text or ''
        for idg in root.iter(NS + 'ItemDefinitionGroup'):
            c = cond_cfg(idg.get('Condition'))
            if c in cfgs:
                for sec in idg:
                    for e in sec:
                        cfgs[c]['idg'][sec.tag[len(NS):] + '.' + e.tag[len(NS):]] = e.text or ''
        files = []
        refs = []
        for ig in root.iter(NS + 'ItemGroup'):
            for it in ig:
                tag = it.tag[len(NS):]
                if tag in ('ClCompile', 'ResourceCompile'):
                    meta = {}
                    for m in it:
                        c = cond_cfg(m.get('Condition'))
                        meta.setdefault(c, {})[m.tag[len(NS):]] = m.text or ''
                    files.append((tag, norm(it.get('Include'), base), meta))
                elif tag == 'ProjectReference':
                    refs.append(os.path.splitext(os.path.basename(it.get('Include')))[0])
        out[name] = {'base': base, 'cfgs': cfgs, 'files': files, 'refs': refs}
    return out


WARN = {'TurnOffAllWarnings': '/W0', 'Level1': '/W1', 'Level2': '/W2', 'Level3': '/W3', 'Level4': '/W4',
        'EnableAllWarnings': '/Wall'}
OPT = {'Disabled': '/Od', 'MaxSpeed': '/O2', 'Full': '/Ox', 'MinSpace': '/O1'}
RT = {'MultiThreadedDebugDLL': '/MDd', 'MultiThreadedDLL': '/MD', 'MultiThreadedDebug': '/MTd', 'MultiThreaded': '/MT'}
DBG = {'ProgramDatabase': '/Zi', 'EditAndContinue': '/ZI', 'OldStyle': '/Z7', 'None': None}
STD = {'stdcpp14': '/std:c++14', 'stdcpp17': '/std:c++17', 'stdcpp20': '/std:c++20', 'stdcpplatest': '/std:c++latest'}


def premake_file_flags(proj, cfg, fpath, meta):
    c = proj['cfgs'][cfg]
    props, idg = c['props'], dict(c['idg'])
    for scope in (None, cfg):
        for k, v in meta.get(scope, {}).items():
            inherited = idg.get('ClCompile.' + k, '')
            idg['ClCompile.' + k] = v.replace('%(' + k + ')', inherited)
    g = lambda k, d='': idg.get('ClCompile.' + k, d)
    is_c = fpath.endswith('.c')
    debuglibs = props.get('UseDebugLibraries') == 'true'
    D = set(x.replace('"', '') for x in split_list(g('PreprocessorDefinitions')))
    cs = props.get('CharacterSet', '')
    if cs == 'Unicode':
        D |= {'UNICODE', '_UNICODE'}
    elif cs == 'MultiByte':
        D |= {'_MBCS'}
    I = [norm(x, proj['base']) for x in split_list(g('AdditionalIncludeDirectories'))]
    EI = [norm(x, proj['base']) for x in split_list(props.get('ExternalIncludePath'))]
    F = set()
    F.add(WARN[g('WarningLevel', 'Level1')])
    if g('TreatWarningAsError') == 'true':
        F.add('/WX')
    for w in split_list(g('DisableSpecificWarnings')):
        F.add('/wd' + w)
    F.add(OPT[g('Optimization', 'Disabled' if debuglibs else 'MaxSpeed')])
    if g('IntrinsicFunctions') == 'true':
        F.add('/Oi')
    if g('FunctionLevelLinking') == 'true':
        F.add('/Gy')
    if g('StringPooling') == 'true':
        F.add('/GF')
    F.add(RT[g('RuntimeLibrary', 'MultiThreadedDebugDLL' if debuglibs else 'MultiThreadedDLL')])
    brc = g('BasicRuntimeChecks', 'EnableFastChecks' if debuglibs else 'Default')
    if brc == 'EnableFastChecks':
        F.add('/RTC1')
    if debuglibs and g('SupportJustMyCode', 'true') == 'true':
        F.add('/JMC')
    d = DBG[g('DebugInformationFormat', 'ProgramDatabase')]
    if d:
        F.add(d)
    if not is_c and g('LanguageStandard'):
        F.add(STD[g('LanguageStandard')])
    if is_c and g('LanguageStandard_C'):
        F.add('/std:' + g('LanguageStandard_C').replace('std', ''))
    if g('ExternalWarningLevel') == 'TurnOffAllWarnings' and EI:
        F.add('/external:W0')
    if True:
        F.add('/EHsc')  # ExceptionHandling Sync (MSBuild passes it to .c too; cl ignores it there)
    F.add('/Zc:inline')  # RemoveUnreferencedCodeData true (MSBuild default)
    F.add('/FC')  # UseFullPaths true (MSBuild default)
    for t in (g('AdditionalOptions') or '').split():
        if not t.startswith('%('):
            F.add(t)
    for fi in split_list(g('ForcedIncludeFiles')):
        F.add('/FI:' + os.path.basename(fi).lower())
    pch = g('PrecompiledHeader', 'NotUsing')
    if pch == 'Use':
        F.add('PCH:use:' + os.path.basename(g('PrecompiledHeaderFile')).lower())
    elif pch == 'Create':  # premake's pch.cpp builds the .pch; CMake builds it from cmake_pch.cxx and compiles pch.cpp with /Yu
        F.add('PCH:use:' + os.path.basename(g('PrecompiledHeaderFile')).lower())
    return {'D': D, 'I': I, 'EI': EI, 'F': F}


# ---------------------------------------------------------------- cmake side
DROP_PREFIX = ('/Fo', '/Fd', '/Fp', '/Fe')


def cmake_file_flags(cmd, src):
    toks = shlex.split(cmd, posix=False)[1:]
    D, I, EI, F = set(), [], [], set()
    pch_hdr = None
    i = 0
    while i < len(toks):
        t = toks[i].strip('"')
        i += 1
        if t.startswith('-'):
            t = '/' + t[1:]
        if t in ('/nologo', '/c', '/FS', '/TP', '/TC') or t.startswith(DROP_PREFIX):
            continue
        if norm(t) == src:
            continue
        if t.startswith('@') and t.endswith('.modmap'):
            F.add('/scanDependencies(modmap)')
        elif t.startswith('/D'):
            D.add(t[2:].replace(chr(92), '').replace('"', ''))
        elif t.startswith('/external:I'):
            v = t[len('/external:I'):] or toks[i].strip('"')
            if not t[len('/external:I'):]:
                i += 1
            EI.append(norm(v))
        elif t.startswith('/I'):
            v = t[2:] or toks[i].strip('"')
            if not t[2:]:
                i += 1
            I.append(norm(v))
        elif t.startswith('/Yu') or t.startswith('/Yc'):
            pch_hdr = t[3:].strip('"')
            F.add(('PCH:use:' if t.startswith('/Yu') else 'PCH:create:') + 'CMAKE_PCH')
        elif t.startswith('/FI'):
            F.add('/FI:' + os.path.basename(t[3:].strip('"')).lower())
        else:
            F.add(t)
    # /Y- ignores every other /Y option: no PCH, and the forced cmake_pch.hxx is the header it wraps.
    if '/Y-' in F:
        F.discard('/Y-')
        F.discard('PCH:use:CMAKE_PCH')
        if '/FI:cmake_pch.hxx' in F and pch_hdr:
            F.discard('/FI:cmake_pch.hxx')
            body = open(pch_hdr).read()
            for inc in re.findall(r'#include\s*[<"]([^>"]+)[>"]', body):
                F.add('/FI:' + os.path.basename(inc).lower())
    return {'D': D, 'I': I, 'EI': EI, 'F': F, 'pch_hdr': pch_hdr}


def cmake_targets(cfg):
    bdir = PREFIX + '-' + cfg
    reply = os.path.join(bdir, '.cmake/api/v1/reply')
    idx = sorted(glob.glob(os.path.join(reply, 'index-*.json')))[-1]
    index = json.load(open(idx))
    cm = json.load(open(os.path.join(reply, index['reply']['codemodel-v2']['jsonFile'])))
    targets = {}
    for t in cm['configurations'][0]['targets']:
        tj = json.load(open(os.path.join(reply, t['jsonFile'])))
        targets[tj['name']] = tj
    cc = json.load(open(os.path.join(bdir, 'compile_commands.json')))
    per_target = collections.defaultdict(dict)
    for e in cc:
        m = re.search(r'CMakeFiles/([^/]+)\.dir/', e['output'].replace('\\', '/'))
        # desert_finalize_targets() splits a target's per-file settings into OBJECT libraries <owner>-<part>,
        # archived into the owner: their sources belong to the owner's premake project.
        owner = re.sub(r'-(Pch|ThirdParty|DebugOptimized)$', '', m.group(1))
        per_target[owner][norm(e['file'])] = e['command']
    return targets, per_target, bdir


def main():
    pm = premake_projects()
    report = {}
    for cfg in CONFIGS:
        targets, cc, bdir = cmake_targets(cfg)
        for name, proj in sorted(pm.items()):
            if cfg not in proj['cfgs']:
                if name in targets and targets[name]['type'] != 'UTILITY':
                    report.setdefault(name, {}).setdefault(cfg, {})['presence'] = 'cmake-only'
                continue
            rep = report.setdefault(name, {}).setdefault(cfg, {})
            if name not in targets:
                rep['presence'] = 'premake-only'
                continue
            pfiles = {}
            for tag, f, meta in proj['files']:
                if tag != 'ClCompile':
                    continue
                if meta.get(cfg, {}).get('ExcludedFromBuild') == 'true' or meta.get(None, {}).get('ExcludedFromBuild') == 'true':
                    continue
                pfiles[f] = premake_file_flags(proj, cfg, f, meta)
            cfiles = {}
            for f, cmd in cc.get(name, {}).items():
                if os.path.basename(f).startswith('cmake_pch'):
                    continue
                cfiles[f] = cmake_file_flags(cmd, f)
            pch_create = [f for f, v in pfiles.items() if any(x.startswith('PCH:create') for x in v['F'])]
            only_p = sorted(set(pfiles) - set(cfiles) - set(pch_create))
            only_c = sorted(set(cfiles) - set(pfiles))
            rep['sources'] = {'premake_only': only_p, 'cmake_only': only_c, 'pch_create_dropped': pch_create}
            diffs = collections.defaultdict(list)
            for f in sorted(set(pfiles) & set(cfiles)):
                p, c = pfiles[f], cfiles[f]
                pF = set(p['F'])
                cF = set(c['F'])
                # CMake's PCH: /Yu<hdr> /FIcmake_pch.hxx  ==  premake /Yu<hdr> /FI<hdr>
                if 'PCH:use:CMAKE_PCH' in cF:
                    cF.discard('PCH:use:CMAKE_PCH')
                    cF.discard('/FI:cmake_pch.hxx')
                    ph = [x for x in pF if x.startswith('PCH:use:')]
                    if ph:
                        pF.discard(ph[0])
                        pF.discard('/FI:' + ph[0][len('PCH:use:'):])
                        cF.add('PCH:use')
                        pF.add('PCH:use')
                key = (
                    ('D-', tuple(sorted(p['D'] - c['D']))), ('D+', tuple(sorted(c['D'] - p['D']))),
                    ('F-', tuple(sorted(pF - cF))), ('F+', tuple(sorted(cF - pF))),
                    ('I', None if p['I'] == c['I'] else (tuple(p['I']), tuple(c['I']))),
                    ('EI', None if p['EI'] == c['EI'] else (tuple(p['EI']), tuple(c['EI']))),
                )
                if any(v for _, v in key):
                    diffs[key].append(f)
            rep['compile'] = [{'files': len(v), 'of': len(set(pfiles) & set(cfiles)), 'example': v[0],
                               'diff': {k: x for k, x in key if x}}
                              for key, v in diffs.items()]
            # link
            idg = proj['cfgs'][cfg]['idg']
            props = proj['cfgs'][cfg]['props']
            t = targets[name]
            plink = {
                'type': props.get('ConfigurationType'),
                'libs': sorted(os.path.basename(x).lower() for x in split_list(idg.get('Link.AdditionalDependencies', '') or idg.get('Lib.AdditionalDependencies', ''))),
                'opts': sorted((idg.get('Link.AdditionalOptions') or '').split()),
                'subsystem': idg.get('Link.SubSystem'),
                'entry': idg.get('Link.EntryPointSymbol'),
                'debug': idg.get('Link.GenerateDebugInformation'),
                'opt_ref': idg.get('Link.OptimizeReferences'),
                'icf': idg.get('Link.EnableCOMDATFolding'),
                'incremental': props.get('LinkIncremental'),
                'refs': sorted(proj['refs']),
                'targetname': (props.get('TargetName') or '') + (props.get('TargetExt') or ''),
                'outdir': norm(props.get('OutDir', '').replace('$(ProjectDir)', proj['base'] + '/')),
            }
            frags = [f['fragment'] for f in t.get('link', {}).get('commandFragments', [])] + \
                    [f['fragment'] for f in t.get('archive', {}).get('commandFragments', [])]
            clink = {
                'type': t['type'],
                'fragments': frags,
                'deps': sorted(targets_by_id(targets).get(d['id'], d['id']) for d in t.get('dependencies', [])),
                'artifacts': [norm(a['path'], os.path.join(WT)) for a in t.get('artifacts', [])],
                'nameOnDisk': t.get('nameOnDisk'),
            }
            rep['link'] = {'premake': plink, 'cmake': clink}
    json.dump(report, open(OUT, 'w'), indent=1, default=list)


_ID = {}


def targets_by_id(targets):
    if not _ID:
        for n, t in targets.items():
            _ID[t['id']] = n
    return _ID


main()
