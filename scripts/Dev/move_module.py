#!/usr/bin/env python3
"""move_module.py <Module> [--dry-run] [--move-reverse] — move one module's files out of the legacy library
(Desert/Desert/Source) into its FOLDER (Desert/<M>/Source/<M>/{Public,Private}), the UE layout (BUILD-MOD-6L).

What it does, all from cmake/DesertModuleTable.cmake (the one table; nothing restated here):
  1. Places every legacy file the way configure does: the desert_placement rows in order, FIRST MATCH WINS, each
     pattern searched in the path relative to the legacy root. The files placed in <Module> are the candidates.
  2. Reverse includes: a candidate #included by a file of an engine module whose dependency closure does not hold
     <Module> (Engine -> Foliage, say). Such a header cannot move yet: configure's MODULE BOUNDARY check and the lower
     module's include roots both refuse <Module/Public/..> from below. By default it stays in the legacy root
     together with every candidate it includes (transitively), and the placement row is narrowed to exactly those
     files; the includes are printed with file:line, to be cut by StartupModule hooks (Engine/Core/ModuleHooks.hpp,
     as Landscape did in 2474606a4), after which a second run moves the rest and deletes the row.
     --move-reverse moves them anyway (row deleted; configure fails until the hooks land).
  3. Splits the movers: a header (or .inl) is Public when a file outside the moving set includes it, or a Public
     header includes it (transitively); every other header and every translation unit is Private.
  4. git mv each file to <FOLDER>/{Public,Private}/<file name> (flat, as Hair and Landscape; two movers with one
     file name abort the run).
  5. Rewrites every #include in the tree (sources of Desert/, Editor/, Runtime/, Server/, Tools/) that resolves to
     a mover into <Dir>/{Public,Private}/<name> (Dir = the FOLDER's last directory), keeping the delimiter; in a
     moved file, a quoted include that resolved relative to its old directory is respelled from its include root.
     Full repo paths (Desert/Desert/Source/<rel>) are rewritten to the new path in every source, CMake, script and
     JSON file (CMake source lists, the PointerOwnership / ModuleBoundary / ShippingPipelines tables, tools).
  6. Deletes the module's desert_placement row (or narrows it, see 2).
  7. Prints the DEPS violations: an #include from a file of <Module> into an engine module outside <Module>'s
     dependency closure (the row's DEPS, transitively). And other mentions of the old paths it did not rewrite.

--dry-run computes and prints all of it and writes nothing. No build is run: after a real run, configure + build.
"""
import argparse, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = "cmake/DesertModuleTable.cmake"
LEGACY = "Desert/Desert/Source"
SOURCE_EXT = (".cpp", ".hpp", ".h", ".inl", ".mm", ".c", ".cc", ".ixx")
HEADER_EXT = (".hpp", ".h", ".inl")
SCAN_TOPS = ("Desert/", "Editor/", "Runtime/", "Server/", "Tools/", "cmake/")
# extra include roots of the tree (besides the legacy root and every module FOLDER's parent)
EXTRA_ROOTS = ("Editor/Resources/Shaders", "Editor/Source")
MENTION_EXT = (".py", ".sh", ".json", ".lua", ".txt", ".cmake", ".yml", ".yaml") + SOURCE_EXT
INCLUDE_RE = re.compile(r'^([ \t]*#[ \t]*include[ \t]*)([<"])([^>"]+)([>"])')


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True, text=True,
                          encoding="utf-8", errors="surrogateescape").stdout


def read_text(rel):
    with open(os.path.join(ROOT, rel), "rb") as f:
        return f.read().decode("utf-8", "surrogateescape")


def write_text(rel, text):
    with open(os.path.join(ROOT, rel), "wb") as f:
        f.write(text.encode("utf-8", "surrogateescape"))


# ── the table ─────────────────────────────────────────────────────────────────────────────────────
class Table:
    def __init__(self):
        self.text = read_text(TABLE)
        self.modules = {}   # name -> {"deps": [...], "folder": str}
        self.order = []
        self.rows = []      # (line index, module, pattern text, compiled)
        for i, line in enumerate(self.text.splitlines()):
            s = line.strip()
            m = re.match(r"desert_module\((\w+)(.*)\)", s)
            if m:
                name, rest = m.group(1), m.group(2)
                folder = re.search(r'FOLDER\s+"([^"]+)"', rest)
                deps = re.search(r"DEPS\s+((?:\w+\s*)+?)(?=\s+(?:UE|FOLDER|REFLECTED|RENDER_FREE)\b|$)", rest)
                self.modules[name] = {
                    "deps": deps.group(1).split() if deps else [],
                    "folder": folder.group(1) if folder else f"Desert/{name}/Source/{name}",
                }
                self.order.append(name)
                continue
            m = re.match(r"desert_placement\((\w+)\s+\[\[(.*)\]\]\)", s)
            if m:
                self.rows.append((i, m.group(1), m.group(2), re.compile(m.group(2))))

    def place(self, rel):
        for _, mod, _, rx in self.rows:
            if rx.search(rel):
                return mod
        return None

    def closure(self, name):
        seen, stack = set(), [name]
        while stack:
            for d in self.modules.get(stack.pop(), {}).get("deps", []):
                if d not in seen:
                    seen.add(d)
                    stack.append(d)
        return seen


# ── the tree ──────────────────────────────────────────────────────────────────────────────────────
def tree_files():
    out = git("ls-files", "-co", "--exclude-standard", "--", *SCAN_TOPS)
    return sorted({p for p in out.splitlines() if "/ThirdParty/" not in p and not p.startswith("Desert/ThirdParty")})


class Tree:
    def __init__(self, table, files):
        self.table = table
        self.files = set(files)
        self.sources = [f for f in files if f.endswith(SOURCE_EXT)]
        self.folder_of = {}  # module -> folder (only those on disk)
        for name in table.order:
            folder = table.modules[name]["folder"]
            if os.path.isdir(os.path.join(ROOT, folder)):
                self.folder_of[name] = folder
        roots = [LEGACY] + sorted({os.path.dirname(f) for f in self.folder_of.values()}) + list(EXTRA_ROOTS)
        self.roots = list(dict.fromkeys(roots))
        self._placed = {}

    def module_of(self, path):
        """The engine module a repo file belongs to, or None (Editor, Tests, Tools ... above every module)."""
        if path in self._placed:
            return self._placed[path]
        mod = None
        if path.startswith(LEGACY + "/"):
            rel = path[len(LEGACY) + 1:]
            mod = "Engine" if rel in ("pch.cpp", "pch.hpp") else self.table.place(rel)
        else:
            for name, folder in self.folder_of.items():
                if path.startswith(folder + "/"):
                    mod = name
                    break
        self._placed[path] = mod
        return mod

    def resolve(self, includer, spelled, quoted):
        """(repo path, True when it resolved relative to the includer's directory) or (None, False)."""
        if quoted:
            cand = os.path.normpath(os.path.join(os.path.dirname(includer), spelled)).replace("\\", "/")
            if cand in self.files:
                return cand, True
        for root in self.roots:
            cand = os.path.normpath(os.path.join(root, spelled)).replace("\\", "/")
            if cand in self.files:
                return cand, False
        return None, False

    def root_spelling(self, path):
        for root in self.roots:
            if path.startswith(root + "/"):
                return path[len(root) + 1:]
        return None


def scan_includes(tree):
    """[(includer, line number, spelled, quoted, target)] for every include that resolves inside the tree."""
    out = []
    for f in tree.sources:
        try:
            text = read_text(f)
        except OSError:
            continue
        for n, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            quoted = m.group(2) == '"'
            target, _ = tree.resolve(f, m.group(3), quoted)
            if target:
                out.append((f, n, m.group(3), quoted, target))
    return out


# ── the plan ──────────────────────────────────────────────────────────────────────────────────────
def plan(module, move_reverse):
    table = Table()
    if module not in table.modules:
        sys.exit(f"move_module: '{module}' is not a desert_module row of {TABLE}")
    rows = [r for r in table.rows if r[1] == module]
    if not rows:
        sys.exit(f"move_module: {TABLE} has no desert_placement({module} ...) row: nothing of {module} is in {LEGACY}")
    tree = Tree(table, tree_files())
    folder = table.modules[module]["folder"]
    inc_dir = os.path.basename(folder)
    candidates = sorted(f for f in tree.files if f.startswith(LEGACY + "/") and tree.module_of(f) == module)
    cand_set = set(candidates)
    includes = scan_includes(tree)
    my_closure = table.closure(module)

    included_by = {}  # target -> [(includer, line, spelled, quoted)]
    for inc, n, sp, q, tgt in includes:
        included_by.setdefault(tgt, []).append((inc, n, sp, q))
    includes_of = {}
    for inc, n, sp, q, tgt in includes:
        includes_of.setdefault(inc, []).append((n, sp, q, tgt))

    # reverse includes: an engine module below <module> includes a candidate
    reverse = []
    for inc, n, sp, q, tgt in includes:
        if tgt not in cand_set or inc in cand_set:
            continue
        r = tree.module_of(inc)
        if r is None or r == module or module in table.closure(r):
            continue
        reverse.append((inc, n, sp, tgt, r))

    stay = set()
    if not move_reverse:
        stack = [t for _, _, _, t, _ in reverse]
        while stack:
            h = stack.pop()
            if h in stay:
                continue
            stay.add(h)
            for _, _, _, tgt in includes_of.get(h, []):
                if tgt in cand_set and tgt not in stay:
                    stack.append(tgt)
    movers = [f for f in candidates if f not in stay]
    mover_set = set(movers)

    public = set()
    for f in movers:
        if f.endswith(HEADER_EXT) and any(inc not in mover_set for inc, _, _, _ in included_by.get(f, [])):
            public.add(f)
    stack = list(public)
    while stack:
        h = stack.pop()
        for _, _, _, tgt in includes_of.get(h, []):
            if tgt in mover_set and tgt.endswith(HEADER_EXT) and tgt not in public:
                public.add(tgt)
                stack.append(tgt)

    dest = {}
    for f in movers:
        side = "Public" if f in public else "Private"
        dest[f] = f"{folder}/{side}/{os.path.basename(f)}"
    by_name = {}
    for f, d in dest.items():
        by_name.setdefault(d, []).append(f)
    clashes = {d: fs for d, fs in by_name.items() if len(fs) > 1}
    existing = [d for d in dest.values() if d in tree.files]

    # include rewrites
    edits = {}  # file -> [(line, old spelling, new spelling)]
    for inc, n, sp, q, tgt in includes:
        new = None
        if tgt in dest:
            new = dest[tgt][len(os.path.dirname(folder)) + 1:]
        elif inc in mover_set:
            _, rel_hit = tree.resolve(inc, sp, q)
            if rel_hit:
                new = tree.root_spelling(tgt)
        if new and new != sp:
            edits.setdefault(inc, []).append((n, sp, new))

    # full repo paths in the build scripts
    cmake_edits = {}
    old_paths = {f: d for f, d in dest.items()}
    path_rx = re.compile("|".join(re.escape(p) for p in sorted(old_paths, key=len, reverse=True))) if old_paths else None
    mentions = []
    rel_rx = re.compile("|".join(re.escape(f[len(LEGACY) + 1:]) for f in sorted(movers, key=len, reverse=True))) if movers else None
    for f in sorted(tree.files):
        if not f.endswith(MENTION_EXT) or f == TABLE:
            continue
        try:
            text = read_text(f)
        except OSError:
            continue
        if path_rx and path_rx.search(text):
            for n, line in enumerate(text.splitlines(), 1):
                for m in path_rx.finditer(line):
                    cmake_edits.setdefault(f, []).append((n, m.group(0), old_paths[m.group(0)]))
        if rel_rx and rel_rx.search(text):
            for n, line in enumerate(text.splitlines(), 1):
                if INCLUDE_RE.match(line) or (path_rx and path_rx.search(line)):
                    continue
                if rel_rx.search(line):
                    mentions.append((f, n, line.strip()[:140]))

    # DEPS violations of the module's own files
    violations = []
    for f in candidates:
        for n, sp, q, tgt in includes_of.get(f, []):
            t = tree.module_of(tgt)
            if t and t != module and t not in my_closure:
                violations.append((f, n, sp, t))

    return dict(table=table, tree=tree, module=module, folder=folder, inc_dir=inc_dir, rows=rows,
                candidates=candidates, stay=sorted(stay), movers=movers, public=public, dest=dest,
                clashes=clashes, existing=existing, edits=edits, cmake_edits=cmake_edits, reverse=reverse,
                violations=violations, mentions=mentions, closure=my_closure)


# ── report / apply ────────────────────────────────────────────────────────────────────────────────
def narrowed_pattern(stay):
    esc = lambda p: re.sub(r"([.\\+*?()\[\]{}|^$])", r"\\\1", p)
    return "^(" + "|".join(esc(f[len(LEGACY) + 1:]) for f in stay) + ")$"


def report(p, move_reverse):
    m = p["module"]
    print(f"== move_module {m}: FOLDER {p['folder']}, DEPS closure {{{', '.join(sorted(p['closure']))}}}")
    print(f"   placement rows: {len(p['rows'])}; files placed in {m}: {len(p['candidates'])}; "
          f"move: {len(p['movers'])} (Public {sum(1 for f in p['movers'] if f in p['public'])}, "
          f"Private {sum(1 for f in p['movers'] if f not in p['public'])}); stay (reverse-included): {len(p['stay'])}")
    print(f"\n-- moves ({len(p['movers'])})")
    for f in p["movers"]:
        print(f"   {f}  ->  {p['dest'][f]}")
    if p["stay"]:
        print(f"\n-- stay in {LEGACY} until the reverse includes are cut ({len(p['stay'])}); the row becomes:")
        for f in p["stay"]:
            print(f"   {f}")
        print(f"   desert_placement({m} [[{narrowed_pattern(p['stay'])}]])")
    else:
        print(f"\n-- the desert_placement({m} ...) row is deleted")
    print(f"\n-- reverse includes, engine module below {m} -> {m} ({len(p['reverse'])}): cut with StartupModule hooks")
    for inc, n, sp, tgt, r in sorted(p["reverse"]):
        gen = "  [generated: DesertHeaderTool output]" if "/Generated/" in inc else ""
        print(f"   {inc}:{n}  [{r}]  #include {sp}{gen}")
    print(f"\n-- DEPS violations: {m}'s files include a module outside its closure ({len(p['violations'])})")
    for f, n, sp, t in p["violations"]:
        print(f"   {f}:{n}  [{t}]  #include {sp}")
    nedits = sum(len(v) for v in p["edits"].values())
    print(f"\n-- #include rewrites: {nedits} in {len(p['edits'])} files")
    for f in sorted(p["edits"]):
        for n, old, new in p["edits"][f]:
            print(f"   {f}:{n}  {old}  ->  {new}")
    ncm = sum(len(v) for v in p["cmake_edits"].values())
    print(f"\n-- full repo path rewrites (build scripts, test tables, tools): {ncm} in {len(p['cmake_edits'])} files")
    for f in sorted(p["cmake_edits"]):
        for n, old, new in p["cmake_edits"][f]:
            print(f"   {f}:{n}  {old}  ->  {new}")
    print(f"\n-- other mentions of the old paths, NOT rewritten ({len(p['mentions'])}): check by hand")
    for f, n, line in p["mentions"]:
        print(f"   {f}:{n}  {line}")
    if p["clashes"]:
        print("\n!! file-name clashes in the flat Public/Private layout (run refused):")
        for d, fs in sorted(p["clashes"].items()):
            print(f"   {d}  <-  {', '.join(fs)}")
    if p["existing"]:
        print("\n!! destination already exists (run refused):")
        for d in p["existing"]:
            print(f"   {d}")


def apply(p):
    if p["clashes"] or p["existing"]:
        sys.exit("move_module: refused, see '!!' above")
    tracked = set(git("ls-files", "--", LEGACY).splitlines())
    # 1. content edits on the old paths (a mover is edited before it moves)
    for f, lst in {**p["edits"]}.items():
        lines = read_text(f).splitlines(keepends=True)
        for n, old, new in lst:
            m = INCLUDE_RE.match(lines[n - 1])
            assert m and m.group(3) == old, f"{f}:{n} changed under the run"
            lines[n - 1] = m.group(1) + m.group(2) + new + m.group(4) + lines[n - 1][m.end():]
        write_text(f, "".join(lines))
    for f in p["cmake_edits"]:
        text = read_text(f)
        for old, new in sorted({(o, nw) for _, o, nw in p["cmake_edits"][f]}, key=lambda x: -len(x[0])):
            text = text.replace(old, new)
        write_text(f, text)
    # 2. the moves
    for f in p["movers"]:
        d = p["dest"][f]
        os.makedirs(os.path.join(ROOT, os.path.dirname(d)), exist_ok=True)
        if f in tracked:
            git("mv", f, d)
        else:
            os.replace(os.path.join(ROOT, f), os.path.join(ROOT, d))
    # 3. the table row
    lines = p["table"].text.splitlines(keepends=True)
    for idx, _, pattern, _ in sorted(p["rows"], reverse=True):
        if p["stay"]:
            lines[idx] = lines[idx].replace(f"[[{pattern}]]", f"[[{narrowed_pattern(p['stay'])}]]", 1)
            p["stay"] = []  # one narrowed row; any further rows of the module go
            continue
        del lines[idx]
    write_text(TABLE, "".join(lines))
    print(f"\nmove_module: applied. Next: configure (the build reads {TABLE}) + build; nothing is committed.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("module")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, write nothing")
    ap.add_argument("--move-reverse", action="store_true",
                    help="move reverse-included headers too and delete the row (configure fails until hooks cut them)")
    a = ap.parse_args()
    p = plan(a.module, a.move_reverse)
    report(p, a.move_reverse)
    if a.dry_run:
        print("\nmove_module: dry run, nothing written.")
        return
    apply(p)


if __name__ == "__main__":
    main()
