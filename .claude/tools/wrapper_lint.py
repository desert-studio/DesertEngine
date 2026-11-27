#!/usr/bin/env python3
"""wrapper_lint — the wrapper checks itself: one rule lives in one file, a document keeps to its budget, nothing stale.

  python .claude/tools/wrapper_lint.py [--quiet-ok] [--repo DIR]

Checks, over CLAUDE.md, .claude/*.md, .claude/agents/*.md, .claude/skills/*/SKILL.md and .claude/tools/*.py:
  1. every .py compiles (in memory: no __pycache__ is written);
  2. every document is within limits.DOC_BUDGET (lines, characters); a skill or an agent file without a budget is an error;
  3. no stale fact (limits-free list STALE below: dead build systems, dead scripts, section numbers of deleted files);
  4. no rule text twice: a line of limits.DUP_LINE_CHARS characters or more may appear in ONE document only;
  5. every `limits.NAME`, skill name, `.claude/tools/x`, `scripts/...` path a document names exists.
Exit 0 and one line when clean; exit 1 and one line per defect otherwise. --quiet-ok prints nothing when clean
(SessionStart). --repo DIR: where to look for files this tree does not carry (the staging copy F:/aftmp/WRAPPER
holds only the changed files; the rest is looked up in the repository).
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import limits

ROOT = os.path.dirname(os.path.dirname(HERE)).replace("\\", "/")

# A fact that was true once. LESSONS.md is exempt: it is the dated history these were moved to.
STALE = [(re.compile(p, re.I), why) for p, why in (
    (r"premake", "premake is gone from the tree (CMake presets)"),
    (r"/opt/homebrew|llvm@18|/usr/local/opt", "tool paths belong to format_on_edit.py (DESERT_CLANG_FORMAT), not to a document"),
    (r"DEV_CONTRACT", "DEV_CONTRACT.md is deleted; name the owning skill"),
    (r"AGENT_BRIEF_|FRAMES\.md|handover/", "deleted document"),
    (r"\baccept\.sh\b|process_event\.py|\bledger\.py\b|night_mode\.sh|wait_bg\.sh|build_wait\.sh|lead_build_win\.sh",
     "deleted script; the command is in studio.py"),
    (r"RunTests\.sh|run_tests\.bat|CheckTidy\.sh", "agents run no suites and no tidy; not an instruction"),
    (r"Docs/Clouds|Resources/Assets/Scenes|Docs/RENDERER_FRAME_STATE", "path that no longer exists"),
    (r"(?<![\w.])-j\s?\d+\b|\bmake\s+-", "a build flag in a document; the build's numbers live in build_land.sh"),
    (r"§\s*\d", "a section number; documents are named by topic («Heading»), numbers rot"),
    (r"ДВА ПАРАМЕТРА ВСЕГДА", "banner repeated per file; the rule lives in desert-engine-architecture / -look"),
    (r"(?<![\w/])/tmp/|/private/tmp", "a temp path of another machine; use the scratch directory"),
)]
# Exempt: the history file, and the programs whose job is to name these things (the guards quote what they refuse,
# studio.py migrate deletes the old scripts, format_on_edit.py owns the formatter's paths).
STALE_EXEMPT = ("LESSONS.md", "wrapper_lint.py", "agent_guard.py", "build_gate.py", "studio.py", "format_on_edit.py")
REF_LIMIT = re.compile(r"\blimits\.([A-Z][A-Z0-9_]+)\b")
REF_SKILL = re.compile(r"\bdesert-engine-[a-z]+\b")
REF_PATH = re.compile(r"(?<![\w/.~-])((?:\.claude/tools|scripts/(?:Dev|CI|MacOS))/[A-Za-z0-9_]+\.(?:py|sh))\b")


def repo_arg():
    if "--repo" in sys.argv:
        return sys.argv[sys.argv.index("--repo") + 1].replace("\\", "/").rstrip("/")
    return None


def docs():
    out = [ROOT + "/CLAUDE.md"]
    out += sorted(glob.glob(ROOT + "/.claude/*.md"))
    out += sorted(glob.glob(ROOT + "/.claude/agents/*.md"))
    out += sorted(glob.glob(ROOT + "/.claude/skills/*/SKILL.md"))
    return [p.replace("\\", "/") for p in out if os.path.isfile(p) and not p.endswith("CODEMAP.md")]


def tools():
    out = sorted(glob.glob(ROOT + "/.claude/tools/*.py")) + sorted(glob.glob(ROOT + "/.claude/skills/*/scripts/*.py"))
    return [p.replace("\\", "/") for p in out]


def rel(p):
    return p[len(ROOT) + 1:]


def read(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def norm(line):
    line = re.sub(r"^\s*(?:[-*]|\d+\.|\|)\s*", "", line.strip())
    return re.sub(r"[\s`*_]+", " ", line).strip().lower()


def exists(path, repo):
    return os.path.exists(ROOT + "/" + path) or bool(repo and os.path.exists(repo + "/" + path))


def main():
    repo = repo_arg()
    bad = []
    py = tools()
    for p in py:
        try:
            compile(read(p), p, "exec")
        except SyntaxError as e:
            bad.append(f"{rel(p)}:{e.lineno}: does not compile — {e.msg}")

    doc = docs()
    seen_budget = set()
    for p in doc:
        r = rel(p)
        body = read(p)
        n, chars = body.count("\n"), len(body)
        budget = limits.DOC_BUDGET.get(r)
        if budget is None and r.startswith(".claude/agents/"):
            budget = limits.DOC_BUDGET["agent"]
        if budget is None:
            bad.append(f"{r}: no line budget — add it to limits.DOC_BUDGET (a document without a budget only grows)")
            continue
        seen_budget.add(r)
        if n > budget[0] or chars > budget[1]:
            bad.append(f"{r}: {n} lines / {chars} characters > budget {budget} — move dated cases to .claude/LESSONS.md, point to the owner "
                       "of a rule instead of repeating it")
    for r, budget in limits.DOC_BUDGET.items():
        if r == "agent" or r in seen_budget:
            continue
        if not repo or not os.path.isfile(repo + "/" + r):
            bad.append(f"{r}: named in limits.DOC_BUDGET but the file does not exist")
        elif read(repo + "/" + r).count("\n") > budget[0] or len(read(repo + "/" + r)) > budget[1]:
            bad.append(f"{r} (in {repo}): over its budget {budget}")

    owner = {}
    for p in doc + py:
        r = rel(p)
        text = read(p)
        base = os.path.basename(p)
        for i, line in enumerate(text.split("\n"), 1):
            if base not in STALE_EXEMPT:
                for rx, why in STALE:
                    m = rx.search(line)
                    if m:
                        bad.append(f"{r}:{i}: stale «{m.group(0)}» — {why}")
            if p.endswith(".md") and base != "LESSONS.md":
                key = norm(line)
                if len(key) >= limits.DUP_LINE_CHARS and not line.lstrip().startswith(("name:", "description:", "tools:")):
                    first = owner.setdefault(key, (r, i))
                    if first[0] != r:
                        bad.append(f"{r}:{i}: the same rule text as {first[0]}:{first[1]} — keep it in one file, "
                                   "name that file in the other")
        if p.endswith(".md"):
            for name in set(REF_LIMIT.findall(text)):
                if not hasattr(limits, name):
                    bad.append(f"{r}: names limits.{name}, which limits.py does not define")
            for name in set(REF_SKILL.findall(text)):
                if not exists(f".claude/skills/{name}/SKILL.md", repo):
                    bad.append(f"{r}: names skill {name}, which does not exist")
            for path in set(REF_PATH.findall(text)):
                if not exists(path, repo):
                    bad.append(f"{r}: names {path}, which does not exist")

    if bad:
        print(f"[wrapper_lint] {len(bad)} defect(s) in the wrapper's own documents — fix before the next launch:")
        print("\n".join("  " + b for b in bad[:40]))
        if len(bad) > 40:
            print(f"  ... +{len(bad) - 40}")
        return 1
    if "--quiet-ok" not in sys.argv:
        print(f"[wrapper_lint] OK: {len(doc)} documents within budget, {len(py)} programs compile, "
              "no stale fact, no rule text in two files")
    return 0


if __name__ == "__main__":
    for _s in (sys.stdout, sys.stderr):
        _s.reconfigure(encoding="utf-8")
    sys.exit(main())
