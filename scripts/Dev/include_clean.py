#!/usr/bin/env python3
"""include_clean.py <header>... [--edit] [--jobs N] | --apply-report <dry-run output> | --repair <build logs,..> <dry-run output> — remove unused #includes (clang-include-cleaner) from the
Engine/Editor .cpp/.hpp files that include one of the given headers (BUILD-MOD-4).

Which files: every Engine/Editor source whose compile-time dependency list (ninja -t deps, i.e. what the compiler
actually opened, by full path) names a given header, plus the headers among those dependencies that #include it
directly. Only REMOVALS are applied: an insertion makes a header heavier, which is the opposite of the goal, and the
build is the proof that nothing a file uses went away.

The compile database is the build's own, rewritten for llvm@18's tooling: Apple clang's PCH binary is unreadable to
it, so `-include-pch X.pch` is dropped and the PCH header it was built from is force-included textually instead
(the file still sees exactly what it saw in the build, and a PCH member is never reported as unused).

SKIP: Windows-zone files (instanced MeshRenderer, shadows, MaterialService, grass, GI, TAA/TSR, sky, ray tracing,
upscalers, D3D12) and generated files are never edited — another machine owns them.
"""
import json, os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build/cmake/macos-clang-ninja")
# the configuration the dev pipeline builds (scripts/Dev/dev_config.txt, read by _common.sh as DEV_CONFIG)
DEV_CONFIG = next(l.split()[1] for l in open(os.path.join(ROOT, "scripts/Dev/dev_config.txt"))
                  if l.split()[:1] == ["Darwin"])
CLEANER = "/opt/homebrew/opt/llvm/bin/clang-include-cleaner"  # llvm@18 never returns from ~ASTContext
SKIP = re.compile(r"/Systems/Scene/Mesh/|/Systems/Scene/Shadows/|MaterialService|/Systems/Scene/Grass/|/GlobalIllumination/"
                  r"|/View/Temporal|/Skybox/|/Sky[A-Z]|/RayTracing|/Systems/RayTracing/|/Upscalers/|/API/D3D12/|\.gen\.")
# Headers whose whole job is a template specialization (rfl::Reflector for glm/UUID): nothing in the including file
# names a symbol from them, so include-cleaner calls them unused, and the reflected type then hits rfl's
# "Unsupported type" static_assert. Never removed.
KEEP = re.compile(r"GlmReflection\.hpp|ReflectionMacros\.hpp")
SCOPE = (os.path.join(ROOT, "Desert/Desert/Source/Engine/"), os.path.join(ROOT, "Editor/Source/Editor/"))

def deps_by_object():
    out = subprocess.run(["ninja", "-C", BUILD, "-f", f"build-{DEV_CONFIG}.ninja", "-t", "deps"], capture_output=True, text=True).stdout
    cur, table = None, {}
    for line in out.splitlines():
        if line and not line.startswith(" "):
            cur = line.split(":")[0]; table[cur] = []
        elif cur and line.strip():
            table[cur].append(os.path.normpath(os.path.join(BUILD, line.strip())))
    return table

def apply_report(report):
    """Applies a dry run's printed removals ("<file>: <hdr> @Line:N; ...") without re-parsing every file: the
    analysis is the expensive part (~12 s a file), and the report is what a reviewer already read."""
    for line in open(report):
        if line.startswith(("FAILED", "files ")) or ": " not in line: continue
        rel, rest = line.rstrip("\n").split(": ", 1)
        path = os.path.join(ROOT, rel)
        if SKIP.search(path): continue
        lines = open(path).read().split("\n")
        drop = set()
        for item in rest.split("; "):
            hdr, _, num = item.rpartition(" @Line:")
            n = int(num) - 1
            name = hdr.strip("<>\"")
            if KEEP.search(name): continue
            if 0 <= n < len(lines) and lines[n].lstrip().startswith("#include") and name in lines[n]: drop.add(n)
            else: print(f"SKIPPED {rel}:{num} {hdr} (line moved)")
        open(path, "w").write("\n".join(l for i, l in enumerate(lines) if i not in drop))
        print(f"{rel}: -{len(drop)}")

INCLUDE_ROOTS = ["Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Desert/UI/Source", "Desert/TextCore/Source",
                 "Desert/Audio/Source", "Desert/PhysicsCore/Source", "Desert/Projects/Source", "Desert/CoreReflection/Source",
                 "Desert/Render2DCore/Source", "Editor/Resources/Shaders"]

def removals(report):
    table = {}
    for line in open(report):
        if line.startswith(("FAILED", "files ")) or ": " not in line: continue
        rel, rest = line.rstrip("\n").split(": ", 1)
        table[os.path.join(ROOT, rel)] = [item.rpartition(" @Line:")[0] for item in rest.split("; ")]
    return table

def reachable(path, seen):
    if path in seen or not os.path.isfile(path): return
    seen.add(path)
    for m in re.finditer(r'^\s*#\s*include\s*[<"]([^">]+)[">]', open(path, errors="ignore").read(), re.M):
        for base in [os.path.dirname(path)] + [os.path.join(ROOT, r) for r in INCLUDE_ROOTS]:
            cand = os.path.normpath(os.path.join(base, m.group(1)))
            if os.path.isfile(cand): reachable(cand, seen); break

def restore_kept(report):
    """Re-adds every KEEP header an earlier apply removed (before KEEP existed)."""
    for path, incs in removals(report).items():
        add = [i for i in incs if KEEP.search(i)]
        if not add or not os.path.isfile(path): continue
        text = open(path).read()
        add = [a for a in add if a not in text]
        if not add: continue
        lines = text.split("\n")
        last = max((i for i, l in enumerate(lines) if re.match(r'\s*#\s*include', l)), default=0)
        lines[last + 1:last + 1] = [f"#include {a}" for a in add]
        open(path, "w").write("\n".join(lines))
        print(f"{os.path.relpath(path, ROOT)}: kept {' '.join(add)}")

def repair(logs, report):
    """A file that stopped compiling used a name it had only through an include the dry run removed — its own, or one
    a header it reaches lost. Each such file gets those includes written into it directly (it uses them, so it names
    them), the edited headers stay light for every other includer."""
    removed = removals(report)
    broken = set()
    for log in logs:
        for m in re.finditer(r"^(/[^:\s]+\.(?:cpp|hpp|h|mm)):\d+:\d+: error:", open(log, errors="ignore").read(), re.M):
            if m.group(1).startswith(ROOT) and "/ThirdParty/" not in m.group(1): broken.add(os.path.normpath(m.group(1)))
    for f in sorted(broken):
        seen = set(); reachable(f, seen)
        want = []
        for h in [f] + sorted(seen - {f}):
            for inc in removed.get(h, []):
                if inc not in want: want.append(inc)
        text = open(f).read()
        present = set(re.findall(r'^\s*#\s*include\s*([<"][^">]+[">])', text, re.M))
        add = [w for w in want if w not in present]
        if not add:
            print(f"NOFIX {os.path.relpath(f, ROOT)}"); continue
        lines = text.split("\n")
        last = max((i for i, l in enumerate(lines) if re.match(r'\s*#\s*include', l)), default=0)
        lines[last + 1:last + 1] = [f"#include {a}" for a in add]
        open(f, "w").write("\n".join(lines))
        print(f"{os.path.relpath(f, ROOT)}: +{len(add)} " + " ".join(add))

def main():
    if "--restore-kept" in sys.argv:
        return restore_kept(sys.argv[sys.argv.index("--restore-kept") + 1])
    if "--repair" in sys.argv:
        i = sys.argv.index("--repair")
        return repair(sys.argv[i + 1].split(","), sys.argv[i + 2])
    if "--apply-report" in sys.argv:
        return apply_report(sys.argv[sys.argv.index("--apply-report") + 1])
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    edit = "--edit" in sys.argv
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 6
    if "--jobs" in sys.argv: args.remove(str(jobs))
    targets = [os.path.join(ROOT, a) if not a.startswith("/") else a for a in args]
    db = json.load(open(os.path.join(BUILD, "compile_commands.json")))
    by_file = {}
    for e in db:
        if f"/{DEV_CONFIG}/" in e["command"] and e["file"] not in by_file: by_file[e["file"]] = e
    table = deps_by_object()
    files = set()
    for obj, deps in table.items():
        if not any(t in deps for t in targets): continue
        src = next((d for d in deps if d.endswith((".cpp", ".mm"))), None)
        if src and src.startswith(SCOPE) and src.endswith(".cpp"): files.add(src)
        for d in deps:  # headers that #include a target directly
            if d.startswith(SCOPE) and d.endswith(".hpp") and d not in targets:
                try:
                    txt = open(d, errors="ignore").read()
                except OSError: continue
                if any(re.search(r'#include\s*[<"][^">]*' + re.escape(os.path.basename(t)) + r'[">]', txt) for t in targets): files.add(d)
    files = sorted(f for f in files if not SKIP.search(f))
    print(f"files {len(files)}", file=sys.stderr, flush=True)
    # Rewritten database: no PCH binary, PCH header force-included; headers borrow the command of a source that uses them.
    entries = []
    for f, e in by_file.items():
        c = re.sub(r"-Xclang -include-pch -Xclang \S+", "", e["command"]).replace("-ftime-trace", "")
        # The tool is a newer clang than the build's: bundled fmt 10.2's consteval format strings are rejected by it,
        # so its checks are compiled out for the tool's parse only (the build keeps them).
        c = c.replace(" -c ", " -DFMT_CONSTEVAL= -c ", 1)
        entries.append({"directory": e["directory"], "file": f, "command": c})
    tmp = tempfile.mkdtemp(prefix="include_clean-")
    json.dump(entries, open(os.path.join(tmp, "compile_commands.json"), "w"))
    def run(f):
        cmd = [CLEANER, "-p", tmp, "--disable-insert", "--print=changes", f]
        if edit: cmd.insert(1, "--edit")
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            return f, r.returncode, r.stdout, r.stderr
        except subprocess.TimeoutExpired as t:
            return f, 124, (t.stdout or b"").decode(), "timed out after 300 s"
    with ThreadPoolExecutor(jobs) as ex:
        for f, rc, out, err in ex.map(run, files):
            rel = os.path.relpath(f, ROOT)
            removed = [l for l in out.splitlines() if l.startswith("- ")]
            if rc != 0 and not removed:
                err = [l for l in err.splitlines() if "error:" in l] or err.strip().splitlines() or ["?"]
                print(f"FAILED {rel}: {err[0][:200]}", flush=True)
            elif removed:
                print(f"{rel}: " + "; ".join(l[2:] for l in removed), flush=True)

main()
