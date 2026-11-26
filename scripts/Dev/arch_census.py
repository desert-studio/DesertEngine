#!/usr/bin/env python3
"""Architecture ratchet: the defects of the coupling and build-cost audits may only go down (STUDIO-AUDIT C1-C5).

    python scripts/Dev/arch_census.py [--rule R] [--list R] [--update] [--accept-rise "why"] [--build <dir>] [--config C]

rules (code):  singleton static-accessor service-locator namespace-state hidden_statics header-state content-name todo
               raw-thread getenv platform-ifdef exception-in-task discarded-result file-too-big
rules (deps):  include-cycle layer-matrix engine-includes-editor     (#include graph; layers = cmake/DesertModuleTable.cmake)
rules (build): header-fanout tu-time duplicate-compile generated-in-source
               (header-fanout, tu-time and duplicate-compile read --build: .ninja_deps, .ninja_log, compile_commands.json)

The baseline scripts/Dev/arch_baseline.json is {rule: {"path:anchor": count}}: the anchor is the place itself (the
offending line, include target or symbol), so a defect moved to another file or a new one beside a removed one is a
rise. --update rewrites it (a vanished entry is dropped; a rise is refused unless --accept-rise states why). A build
rule's entries are carried over unchanged by an --update run without --build. header-fanout has no baseline: its
ceilings are scripts/Dev/header_fanout_budget.txt. Exit 0 = no rise, 2 = a rise (the place and the rule's why are
printed), 1 = a usage or input error.
"""
import json
import os
import re
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()
BASE = os.path.join(ROOT, "scripts", "Dev", "arch_baseline.json")
BUDGET = os.path.join(ROOT, "scripts", "Dev", "header_fanout_budget.txt")
MODULE_TABLE = os.path.join(ROOT, "cmake", "DesertModuleTable.cmake")
LEGACY_ROOT = "Desert/Desert/Source/"
SOURCE = re.compile(r"\.(hpp|cpp|h|inl|mm|glslh)$")
ENGINE = re.compile(r"^Desert/(?!Tests/|Projects/).*\.(hpp|cpp|h)$")
# Tables of numbers and icon names are data, not code.
CODE = re.compile(r"^(Desert|Editor|Runtime|Server)/(?!Tests/|Projects/|.*Generated/|.*(?:SMAAAreaTexData|IconsMaterialDesignIcons)\.h)"
                  r".*\.(hpp|cpp|h)$")
# The sources whose worlds share a process.
STATICS = re.compile(r"^(Desert/(Desert/Source/Engine|Common/Source/Common/Core|UI|Audio|Landscape|Water|VFX)|Editor/Source|Runtime)/.*\.(hpp|cpp|h)$")
# Headers whose state a module DLL could copy: each DLL that ODR-uses a header-defined variable holds its own copy.
HEADERS = re.compile(r"^(Desert|Editor)/(?!Tests/|Projects/|.*Generated/|.*ThirdParty/).*\.(hpp|h|inl)$")
# The one-line finding that every referencing TU sits in the header's owner module (design BINT-STATE, choice (v)).
HEADER_STATE_FINDING = "header-state:"
INLINE_VAR = re.compile(r"\b(?:inline\s+(?:static\s+|thread_local\s+)?|(?:static|thread_local)\s+inline\s+)(?!namespace\b)")
TEMPLATE_STATIC = re.compile(r"^\s*(?:template\s*<.*>\s*)?[\w:<>,\s*&]+?\s[\w:]*\w\s*<[^()]*>\s*::\s*\w+\s*(?:=(?!=)|\{|;)")
# The platform layer: the one place an #if on the operating system belongs.
PLATFORM = re.compile(r"(^|/)(Platform|Platforms|Windows|Win32|MacOS|Apple|Linux|Posix)(/|$)|/Platform\w*\.(hpp|cpp|h|mm)$")
MAX_LINES = 2500
TU_SECONDS = 30

# rule -> (files, pattern, why). A hit is a line; its anchor is the line's text, so the place is the code itself.
# Common::Singleton<T>'s storage accessor (std::unique_ptr<T>& Instance()) is not a second singleton: the template
# is counted at its GetInstance and at every class that derives from it.
RULES = {
    "singleton": (ENGINE, re.compile(r"\bstatic\s+(?!std::unique_ptr<T>\s*&\s*Instance\b)[A-Za-z_][\w:<>]*\s*&\s*(Instance|GetInstance)\s*\(\s*\)|"
                                     r":\s*public\s+[\w:]*Singleton\s*<"),
                  "a process-wide object breaks two worlds in one process; pass the owner"),
    "static-accessor": (CODE, re.compile(r"\bstatic\s+[A-Za-z_][\w:<>]*\s*&\s*Get\w*\s*\(\s*\)"),
                        "a static Get() is a hidden global; the caller is handed what it uses"),
    "service-locator": (CODE, re.compile(r"\bGetSubsystem\s*<"),
                        "a lookup by type hides the dependency; take it in the constructor"),
    "namespace-state": (STATICS, re.compile(r"^\s*(?!.*\b(?:const|constexpr|constinit|extern|return|using)\b)"
                                            r"(?:(?:static|inline|thread_local)\s+)*[A-Za-z_][\w:]*(?:<.*>)?[\s*]+"
                                            r"g_\w+\s*(?:\[[^\]]*\]\s*)?(?:[;{=]|$)", re.M),
                        "mutable state at namespace scope has no owner and is shared by every world"),
    "content-name": (ENGINE, re.compile(r'"[^"\n]*\b(Bistro|Downtown|GI_Bistro_Night|Sandbox)\w*[^"\n]*"'),
                     "the engine names a game's content; content names live in content"),
    "todo": (CODE, re.compile(r"//.*\b(TODO|FIXME|HACK)\b"), "unfinished work does not ship"),
    "raw-thread": (CODE, re.compile(r"\bstd::j?thread\b(?!::)"),
                   "a thread outside the JobSystem escapes its scheduling, budgets and shutdown"),
    "getenv": (CODE, re.compile(r"(?<![\w:])(?:std::)?getenv\s*\(|\b_dupenv_s\s*\(|\bGetEnvironmentVariable[AW]?\s*\("),
               "the environment is an unowned global input; settings and the command line are owned"),
    "platform-ifdef": (CODE, re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b(?:_WIN32|_WIN64|WIN32|__APPLE__|__MACH__|"
                                        r"__linux__|TARGET_OS_\w+|DESERT_PLATFORM_\w+)\b"),
                       "an operating-system branch outside the platform layer splits one mechanism in two"),
    "exception-in-task": (CODE, re.compile(r"\btry\s*\{|\bcatch\s*\(|\bthrow\b(?!\s*\(\s*\))"),
                          "errors are values: an exception out of a task or a destructor terminates the process"),
    "discarded-result": (CODE, re.compile(r"\(\s*void\s*\)\s*[\w:.>-]+\s*\(|\bstatic_cast\s*<\s*void\s*>\s*\(|"
                                          r"\bstd::ignore\s*="),
                         "a discarded result is an error nobody handles"),
    # An engine library reaches up into the editor.
    "engine-includes-editor": (ENGINE, re.compile(r'#include\s*[<"](Editor|EditorCore)/'),
                               "an engine library may not reach up into the editor"),
}
WHY = {
    **{r: w for r, (_, _, w) in RULES.items()},
    "hidden_statics": "a function-local mutable static is state with no owner, shared by every world",
    "file-too-big": f"a file over {MAX_LINES} lines is several responsibilities; split it (counted per 100 lines)",
    "include-cycle": "headers that include each other cannot be understood or compiled alone (SF.9)",
    "layer-matrix": "a module includes a module that is not among its DEPS in cmake/DesertModuleTable.cmake",
    "header-fanout": "a header over its ceiling in scripts/Dev/header_fanout_budget.txt rebuilds too many objects",
    "tu-time": f"a first-party translation unit compiles longer than {TU_SECONDS} s",
    "duplicate-compile": "a first-party source compiled by more than one target; a tool links the library instead",
    "generated-in-source": "generated code is tracked in the source tree; it belongs in the build tree",
    "header-state": "mutable state defined in a header has one copy per module DLL; define it in the owner's .cpp "
                    "behind a non-inline accessor, or write the one-line 'header-state:' finding that every user "
                    "sits in the owner module",
}
ORDER = ["singleton", "static-accessor", "service-locator", "namespace-state", "hidden_statics", "header-state", "content-name", "todo",
         "raw-thread", "getenv", "platform-ifdef", "exception-in-task", "discarded-result", "file-too-big",
         "include-cycle", "layer-matrix", "engine-includes-editor",
         "header-fanout", "tu-time", "duplicate-compile", "generated-in-source"]
BUILD_RULES = {"header-fanout", "tu-time", "duplicate-compile"}
EXCLUDE = {"raw-thread": re.compile(r"JobSystem"), "platform-ifdef": PLATFORM}

NOT_CODE = re.compile(r'R"(?P<d>[^ ()\\\t\n]*)\(.*?\)(?P=d)"|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"'
                      r"|(?<![0-9A-Fa-f])'(?:\\.|[^'\\\n])+'", re.S)
DIRECTIVE = re.compile(r"^\s*#\s*(\w+)")
TOKEN = re.compile(r"[{};]|\bstatic\b")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)


def anchor(text):
    return re.sub(r"\s+", " ", text.strip())[:120]


def hidden_statics(text):
    """The lines of function-local statics that are not const: state with no owner, shared by every world."""
    text = NOT_CODE.sub(lambda m: '""' + "\n" * m.group().count("\n"), text)
    lines, branches, continued = text.split("\n"), [], False
    for i, line in enumerate(lines):
        directive = DIRECTIVE.match(line)
        if directive and not continued:
            word = directive.group(1)
            if word.startswith("if"):
                branches.append(True)
            elif word.startswith("el") and branches:
                branches[-1] = False  # one branch only, so the braces of the two do not add up
            elif word == "endif" and branches:
                branches.pop()
        blank = directive or continued or not all(branches)
        continued = (directive is not None or continued) and line.rstrip().endswith("\\")
        if blank:
            lines[i] = ""
    text = "\n".join(lines)
    found, scopes, start = [], [], 0
    for m in TOKEN.finditer(text):
        token = m.group()
        if token == "static":
            if scopes and scopes[-1] == "function":
                head = re.split(r"[=({;]", text[m.end():m.end() + 400], maxsplit=1)[0]
                while "<" in head and re.search(r"<[^<>]*>", head):
                    head = re.sub(r"<[^<>]*>", "", head)
                # A const pointee does not make the pointer const: only what follows the last '*' counts.
                own = head.rsplit("*", 1)[-1] if not re.search(r"\b(constexpr|constinit)\b", head) else head
                if not re.search(r"\b(const|constexpr|constinit)\b", own):
                    found.append(text.count("\n", 0, m.start()) + 1)
            continue
        if token == "{":
            head = text[start:m.start()]
            while re.search(r"<[^<>]*>", head):
                head = re.sub(r"<[^<>]*>", "", head)
            if re.search(r"\b(namespace|extern)\b", head):
                scopes.append("namespace")
            elif re.search(r"\b(class|struct|union|enum)\b", head) and not re.search(r"\)\s*(const|noexcept|override)?\s*$", head):
                scopes.append("class")
            elif re.search(r"\bSingleton\s*::\s*Instance\s*\(\s*\)\s*$", head):
                # The owner's definition of Common::Singleton<X>::Instance(): the singleton's one storage, which
                # the singleton rule already counts at the class.
                scopes.append("singleton-storage")
            else:
                scopes.append("function")
        elif token == "}" and scopes:
            scopes.pop()
        start = m.end()
    return found


def header_state(text):
    """The lines of mutable variables a header defines: inline variables and members, template static members and
    function-local statics of the header's (inline) functions. Each module DLL that ODR-uses one gets its own copy."""
    stripped = NOT_CODE.sub(lambda m: '""' + "\n" * m.group().count("\n"), text)
    found = set(hidden_statics(text))
    for m in INLINE_VAR.finditer(stripped):
        head = re.split(r"[=({;]", stripped[m.end():m.end() + 400], maxsplit=1)[0]
        stop = stripped[m.end() + len(head):][:1]
        if (stop != "(" and head.strip() and not re.search(r"\boperator\s*$", head) and not re.search(r"\b(constexpr|constinit|using|class|struct|enum)\b", head)
                and not re.search(r"\bconst\b", head.rsplit("*", 1)[-1])):
            found.add(stripped.count("\n", 0, m.start()) + 1)
    code = stripped.split("\n")
    for i, line in enumerate(code):
        previous = code[i - 1] if i else ""
        if (TEMPLATE_STATIC.match(line) and ("template" in line or re.match(r"^\s*template\b", previous))
                and not re.search(r"\b(const|constexpr|constinit|using|typedef|return|friend)\b", line)):
            found.add(i + 1)
    raw = text.split("\n")
    return sorted(n for n in found
                  if not any(HEADER_STATE_FINDING in raw[k] for k in (n - 1, n - 2) if 0 <= k < len(raw)))


def add(hits, rule, path, line, key):
    hits.setdefault(rule, {}).setdefault(f"{path}:{key}", []).append(line)


def tracked():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "Desert", "Editor", "Runtime", "Server", "Tools"],
                         capture_output=True, text=True, encoding="utf-8", check=True).stdout.split("\n")
    return [f for f in out if f and SOURCE.search(f) and not f.startswith("ThirdParty/")]


def read(path):
    try:
        return open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read()
    except OSError:
        return None


def scan_code(files, texts, hits):
    for f in files:
        if not CODE.match(f):
            continue
        text = texts[f]
        lines = text.split("\n")
        for rule, (where, rx, _) in RULES.items():
            if not where.match(f) or (rule in EXCLUDE and EXCLUDE[rule].search(f)) or not rx.search(text):
                continue
            for i, line in enumerate(lines):
                if rule != "todo" and line.lstrip().startswith("//"):
                    continue
                m = rx.search(line)
                if m:
                    add(hits, rule, f, i + 1, anchor(line))
        if STATICS.match(f) and "static" in text:
            for n in hidden_statics(text):
                add(hits, "hidden_statics", f, n, anchor(lines[n - 1]))
        if HEADERS.match(f) and re.search(r"\b(static|inline|template)\b", text):
            for n in header_state(text):
                add(hits, "header-state", f, n, anchor(lines[n - 1]))
        if len(lines) > MAX_LINES:
            # Counted in steps of 100 lines, so a big file may not keep growing but a line or two is no event.
            for _ in range((len(lines) - MAX_LINES) // 100 + 1):
                add(hits, "file-too-big", f, 1, "size")


def generated_in_source(files, hits):
    for f in files:
        if "/Generated/" in f and ".gen." in f:
            add(hits, "generated-in-source", f, 1, "tracked")


# ── #include graph ──────────────────────────────────────────────────────────────────────────────────────────
def include_graph(files, texts):
    """file -> [(line, target file)] for every #include that resolves to a tracked first-party file."""
    known = set(files)
    roots = sorted({f[:f.index("/Source/") + len("/Source/")] for f in files if "/Source/" in f} |
                   {"Editor/Resources/Shaders/"}, key=len, reverse=True)
    by_rel = {}
    for f in files:
        for r in roots:
            if f.startswith(r):
                by_rel.setdefault(f[len(r):], []).append(f)
    graph = {}
    for f in files:
        text = texts.get(f)
        if not text or "#" not in text:
            continue
        own = next((r for r in roots if f.startswith(r)), "")
        edges = []
        for m in INCLUDE.finditer(text):
            target = None
            if m.group(1) == '"':
                local = os.path.normpath(os.path.join(os.path.dirname(f), m.group(2))).replace("\\", "/")
                target = local if local in known else None
            if target is None:
                cands = by_rel.get(m.group(2), [])
                target = next((c for c in cands if own and c.startswith(own)), cands[0] if cands else None)
            if target and target != f:
                edges.append((text.count("\n", 0, m.start()) + 1, target))
        graph[f] = edges
    return graph


def include_cycles(graph, hits):
    """Tarjan's strongly connected components; every include edge inside a component of two or more is a hit."""
    index, low, stack, on, comp, counter = {}, {}, [], set(), {}, [0]
    for start in graph:
        if start in index:
            continue
        work = [(start, iter(graph.get(start, [])))]
        index[start] = low[start] = counter[0]
        counter[0] += 1
        stack.append(start)
        on.add(start)
        while work:
            node, it = work[-1]
            advanced = False
            for _, nxt in it:
                if nxt not in index:
                    index[nxt] = low[nxt] = counter[0]
                    counter[0] += 1
                    stack.append(nxt)
                    on.add(nxt)
                    work.append((nxt, iter(graph.get(nxt, []))))
                    advanced = True
                    break
                if nxt in on:
                    low[node] = min(low[node], index[nxt])
            if advanced:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[node])
            if low[node] == index[node]:
                members = []
                while True:
                    w = stack.pop()
                    on.discard(w)
                    members.append(w)
                    if w == node:
                        break
                for w in members:
                    comp[w] = node if len(members) > 1 else None
    for f, edges in graph.items():
        for line, target in edges:
            if comp.get(f) and comp.get(f) == comp.get(target):
                add(hits, "include-cycle", f, line, target)


def module_table():
    """(modules {name: (deps, folder)}, placement [(regex, module)]) from cmake/DesertModuleTable.cmake."""
    text = open(MODULE_TABLE, encoding="utf-8").read()
    modules, reflected, rows = {}, [], []
    for m in re.finditer(r"^desert_module\((\w+)(.*)\)\s*$", text, re.M):
        name, rest = m.group(1), m.group(2)
        deps = re.search(r"\bDEPS\s+(.*?)(?=\bUE\b|\bFOLDER\b|\bREFLECTED\b|\bRENDER_FREE\b|$)", rest)
        folder = re.search(r'\bFOLDER\s+"([^"]+)"', rest)
        modules[name] = (deps.group(1).split() if deps else [],
                         folder.group(1) if folder else f"Desert/{name}/Source/{name}")
        if re.search(r"\bREFLECTED\b", rest):
            reflected.append(name)
    for kind in ("Reflection", "Components"):
        rows += [(re.compile(rf"Engine/Generated/{kind}_{n}\.gen\.cpp$"), n) for n in reflected]
    rows += [(re.compile(m.group(2)), m.group(1))
             for m in re.finditer(r"^desert_placement\((\w+)\s+\[\[(.*?)\]\]\)", text, re.M)]
    return modules, rows


def layer_matrix(graph, hits):
    modules, rows = module_table()
    closure = {}

    def reach(name):
        if name not in closure:
            closure[name] = {name}
            for d in modules[name][0]:
                closure[name] |= reach(d)
        return closure[name]

    folders = sorted(((spec[1].rstrip("/") + "/", n) for n, spec in modules.items()), key=lambda x: -len(x[0]))

    def module_of(f):
        if f.startswith(LEGACY_ROOT):
            rel = f[len(LEGACY_ROOT):]
            return "Engine" if rel in ("pch.cpp", "pch.hpp") else next((n for rx, n in rows if rx.search(rel)), None)
        return next((n for d, n in folders if f.startswith(d)), None)

    for f, edges in graph.items():
        own = module_of(f)
        if not own:
            continue
        allowed = reach(own)
        for line, target in edges:
            other = module_of(target)
            if other and other not in allowed:
                add(hits, "layer-matrix", f, line, f"{target} ({own} -> {other})")


# ── build tree ──────────────────────────────────────────────────────────────────────────────────────────────
class Build:
    def __init__(self, path, config):
        self.dir = os.path.abspath(path)
        cache = os.path.join(self.dir, "CMakeCache.txt")
        if not os.path.isfile(cache):
            raise SystemExit(f"arch_census: --build {path}: no CMakeCache.txt")
        values = dict(re.findall(r"^([A-Za-z_]\w*):[A-Z]+=(.*)$", open(cache, encoding="utf-8", errors="replace").read(), re.M))
        self.home = os.path.normpath(values.get("CMAKE_HOME_DIRECTORY", ROOT))
        # The ninja that configured the tree, never one from PATH (a Windows shell has none).
        self.ninja = values.get("CMAKE_MAKE_PROGRAM", "")
        multi = values.get("CMAKE_CONFIGURATION_TYPES", "")
        self.config = config
        self.dir_part = f".dir/{config}/" if multi else ".dir/"
        name = f"build-{config}.ninja" if multi else "build.ninja"
        self.ninja_file = os.path.join(self.dir, name)
        if not os.path.isfile(self.ninja_file):
            raise SystemExit(f"arch_census: --build {path}: no {name}")
        cc = os.path.join(self.dir, "compile_commands.json")
        self.commands = [c for c in json.load(open(cc, encoding="utf-8")) if self.dir_part in c.get("output", "").replace("\\", "/")] \
            if os.path.isfile(cc) else []

    def rel(self, p):
        a = os.path.normpath(p if os.path.isabs(p) else os.path.join(self.dir, p))
        r = os.path.relpath(a, self.home).replace("\\", "/") if os.path.splitdrive(a)[0].lower() == os.path.splitdrive(self.home)[0].lower() else ".."
        return None if r.startswith("..") or r.startswith("build/") else r

    def out_key(self, p):
        return os.path.normcase(os.path.normpath(p if os.path.isabs(p) else os.path.join(self.dir, p)))


def first_party(rel):
    return rel is not None and not rel.startswith("ThirdParty/")


def header_fanout(build, hits, report):
    run = subprocess.run([build.ninja, "-C", build.dir, "-f", os.path.basename(build.ninja_file), "-t", "deps"],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    if run.returncode != 0:
        raise SystemExit(f"arch_census: header-fanout: {build.ninja} -t deps failed: {run.stderr.strip()[:200]}")
    deps, cur = {}, None
    for line in run.stdout.split("\n"):
        if not line.startswith(" "):
            cur = line.split(": #deps", 1)[0] if ": #deps" in line else None
            if cur:
                deps[cur] = []
        elif cur and line.strip():
            deps[cur].append(line.strip())
    # A precompiled header's headers reach every object compiled with it. clang: the object names the PCH's header
    # and the PCH's record is <header>.pch; MSVC: the /Yc object's record holds them and /Yu objects name its /Fp.
    pch = {o.rsplit(".", 1)[0]: d for o, d in deps.items() if o.endswith((".pch", ".gch"))}
    by_out = {build.out_key(o): o for o in deps}
    yc, yu = {}, {}
    for c in build.commands:
        cmd = c.get("command", "")
        fp = re.search(r'[/-]Fp"?([^"\s]+)', cmd)
        if fp and re.search(r"[/-]Yc", cmd):
            yc[fp.group(1)] = deps.get(by_out.get(build.out_key(c["output"])), [])
        elif fp and re.search(r"[/-]Yu", cmd):
            yu[build.out_key(c["output"])] = fp.group(1)
    count, objects = {}, [o for o in deps if o.endswith((".o", ".obj")) and build.dir_part in o.replace("\\", "/")]
    for o in objects:
        hs = set(deps[o])
        for d in deps[o]:
            hs.update(pch.get(d, []))
        hs.update(yc.get(yu.get(build.out_key(o)), []))
        for r in {build.rel(d) for d in hs}:
            if r:
                count[r] = count.get(r, 0) + 1
    budget = [l.split("#", 1)[0].split() for l in open(BUDGET, encoding="utf-8")] if os.path.isfile(BUDGET) else []
    for row in (b for b in budget if len(b) == 2):
        header, ceiling = row[0], int(row[1])
        n = count.get(header, 0)
        report.append(f"  header-fanout: {header} {n}/{ceiling} objects")
        if not os.path.exists(os.path.join(ROOT, header)):
            add(hits, "header-fanout", header, 1, "not in the tree")
        elif n > ceiling:
            add(hits, "header-fanout", header, 1, f"{n} > {ceiling}")


def tu_time(build, hits):
    log = os.path.join(build.dir, ".ninja_log")
    if not os.path.isfile(log):
        raise SystemExit(f"arch_census: tu-time: no {log}")
    source = {build.out_key(c["output"]): (c["file"], c["output"]) for c in build.commands}
    last = {}
    for line in open(log, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) == 5 and parts[0].isdigit():
            last[build.out_key(parts[3])] = int(parts[1]) - int(parts[0])
    for out, ms in last.items():
        if ms > TU_SECONDS * 1000 and out in source:
            rel = build.rel(source[out][0])
            target = re.search(r"([^/\\]+)\.dir[/\\]", source[out][1])
            if first_party(rel):
                add(hits, "tu-time", rel, 1, target.group(1) if target else "?")


def duplicate_compile(build, hits):
    targets = {}
    for c in build.commands:
        rel = build.rel(c["file"])
        target = re.search(r"([^/\\]+)\.dir[/\\]", c["output"])
        if first_party(rel) and target and SOURCE.search(rel):
            targets.setdefault(rel, set()).add(target.group(1))
    for rel, ts in targets.items():
        for t in sorted(ts)[1:]:
            add(hits, "duplicate-compile", rel, 1, t)


def arg(name):
    if name not in sys.argv:
        return None
    at = sys.argv.index(name) + 1
    if at >= len(sys.argv) or sys.argv[at].startswith("--"):
        raise SystemExit(f"arch_census: {name} needs a value")
    return sys.argv[at]


def main():
    if not os.path.isdir(ROOT + "/Desert"):
        sys.exit("arch_census: no Desert/ here")
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    only, listed, build_dir, config = arg("--rule"), arg("--list"), arg("--build"), arg("--config") or "Release"
    for r in (only, listed):
        if r and r not in ORDER:
            print(f"arch_census: unknown rule '{r}' (rules: {' '.join(ORDER)})")
            return 1
    files = tracked()
    texts = {f: read(f) or "" for f in files}
    hits, report = {}, []
    scan_code(files, texts, hits)
    generated_in_source(files, hits)
    graph = include_graph(files, texts)
    include_cycles(graph, hits)
    layer_matrix(graph, hits)
    build = Build(build_dir, config) if build_dir else None
    if build:
        header_fanout(build, hits, report)
        tu_time(build, hits)
        duplicate_compile(build, hits)
    if listed:
        for key, ls in sorted(hits.get(listed, {}).items()):
            path, place = key.split(":", 1)
            print(f"{path}:{ls[0]}: {place}" + (f" (x{len(ls)})" if len(ls) > 1 else ""))
        return 0
    now = {r: {k: len(v) for k, v in sorted(d.items())} for r, d in hits.items()}
    base = json.load(open(BASE, encoding="utf-8")) if os.path.exists(BASE) else None
    if base is not None and not all(isinstance(v, dict) and all(isinstance(n, int) for n in v.values())
                                    for k, v in base.items() if not k.startswith("_")):
        print("arch_census: arch_baseline.json is not {rule: {\"path:anchor\": count}}")
        return 1
    ran = [r for r in ORDER if (build or r not in BUILD_RULES) and (only is None or r == only)]
    rises = [(r, k, (base or {}).get(r, {}).get(k, 0), n) for r in ran if r != "header-fanout"
             for k, n in now.get(r, {}).items() if n > (base or {}).get(r, {}).get(k, 0)]
    rises += [("header-fanout", k, 0, n) for k, n in now.get("header-fanout", {}).items() if "header-fanout" in ran]
    if "--update" in sys.argv:
        if only:
            print("arch_census: --update rewrites every rule; drop --rule")
            return 1
        if rises and "--accept-rise" not in sys.argv:
            print(f"arch_census: {len(rises)} rise(s), baseline not written")
            return 2
        reasons = list((base or {}).get("_accepted_rises", []))
        if rises:
            reasons.append(arg("--accept-rise"))
        out = {r: now.get(r, {}) for r in ORDER if r != "header-fanout" and r in ran}
        # A build rule not measured by this run keeps its baseline.
        out.update({r: (base or {}).get(r, {}) for r in BUILD_RULES - {"header-fanout"} if r not in ran})
        out = {r: d for r, d in out.items() if d}
        with open(BASE, "w", encoding="utf-8", newline="\n") as f:
            json.dump({**out, "_accepted_rises": reasons} if reasons else out, f, indent=1, sort_keys=True)
            f.write("\n")
        print("arch_census: baseline " + " ".join(f"{r}={sum(d.values())}" for r, d in sorted(out.items())))
        return 0
    if base is None:
        print("arch_census: no baseline")
        return 1
    for r in ORDER:
        if only and r != only:
            continue
        if r not in ran:
            print(f"{r:24} not measured: needs --build <dir>")
            continue
        n, b = sum(now.get(r, {}).values()), sum(base.get(r, {}).values())
        print(f"{r:24} {n:5}" + ("" if r == "header-fanout" else f"  (baseline {b})"))
    for line in report:
        print(line)
    for r, key, was, n in rises:
        path, place = key.split(":", 1)
        print(f"arch_census: RISE {r}: {path}:{hits[r][key][0]}: {place} ({was} -> {n}) — {WHY[r]}")
    print("arch_census:", "FAILED" if rises else "ok")
    return 2 if rises else 0


if __name__ == "__main__":
    sys.exit(main())
