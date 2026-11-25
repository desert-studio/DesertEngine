#!/usr/bin/env python3
"""Architecture ratchet: the counted defects of the coupling audit may only go down.

    arch_census.py              compare the tree with scripts/Dev/arch_baseline.json; exit 1 on any new hit
    arch_census.py --update     rewrite the baseline; refuses a rise unless --accept-rise "<reason>" states why
    arch_census.py --list RULE  print the hits of one rule

Counts are per rule and per file, so a hit cannot be paid for by removing one elsewhere. A moved file is a fall in one
and a rise in the other: run --update with --accept-rise "moved A -> B".
"""
import json
import os
import re
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()
BASE = os.path.join(ROOT, "scripts", "Dev", "arch_baseline.json")
ENGINE = re.compile(r"^Desert/(?!Tests/|Projects/).*\.(hpp|cpp|h)$")
# Tables of numbers and icon names are data, not code.
CODE = re.compile(r"^(Desert|Editor|Runtime|Server)/(?!Tests/|Projects/|.*Generated/|.*(?:SMAAAreaTexData|IconsMaterialDesignIcons)\.h)"
                  r".*\.(hpp|cpp|h)$")
# The sources whose worlds share a process.
STATICS = re.compile(r"^Desert/(Desert/Source/Engine|Common/Source/Common/Core|UI|Audio)/.*\.(hpp|cpp|h)$")
MAX_LINES = 2500
RULES = {
    # A process-wide object: breaks two worlds in one process (audit 8, 13, 14).
    "singleton": (ENGINE, re.compile(r"\bstatic\s+[A-Za-z_][\w:<>]*\s*&\s*(Get|Instance|GetInstance)\s*\(\s*\)|"
                                     r":\s*public\s+[\w:]*Singleton\s*<")),
    # The engine knows a game's content by name.
    "content-name": (ENGINE, re.compile(r'"[^"\n]*\b(Bistro|Downtown|GI_Bistro_Night|Sandbox)\w*[^"\n]*"')),
    # An engine library reaches up into the editor.
    "engine-includes-editor": (ENGINE, re.compile(r'#include\s*[<"](Editor|EditorCore)/')),
    "todo": (CODE, re.compile(r"//.*\b(TODO|FIXME|HACK)\b")),
}


NOT_CODE = re.compile(r'R"(?P<d>[^ ()\\\t\n]*)\(.*?\)(?P=d)"|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"'
                      r"|(?<![0-9A-Fa-f])'(?:\\.|[^'\\\n])+'", re.S)
DIRECTIVE = re.compile(r"^\s*#\s*(\w+)")
TOKEN = re.compile(r"[{};]|\bstatic\b")


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
                if not re.search(r"\b(const|constexpr|constinit)\b", head):
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
            else:
                scopes.append("function")
        elif token == "}" and scopes:
            scopes.pop()
        start = m.end()
    return found


def scan():
    files = subprocess.run(["git", "-C", ROOT, "ls-files", "Desert", "Editor", "Runtime", "Server"],
                           capture_output=True, text=True, encoding="utf-8", check=True).stdout.split("\n")
    hits = {}
    for f in files:
        if not CODE.match(f):
            continue
        try:
            text = open(os.path.join(ROOT, f), encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        lines = text.split("\n")
        for rule, (where, rx) in RULES.items():
            if where.match(f) and rx.search(text):
                found = [i + 1 for i, l in enumerate(lines) if rx.search(l)
                         and not (rule != "todo" and l.lstrip().startswith("//"))]
                if found:
                    hits.setdefault(rule, {})[f] = found
        if STATICS.match(f) and "static" in text:
            found = hidden_statics(text)
            if found:
                hits.setdefault("hidden_statics", {})[f] = found
        if len(lines) > MAX_LINES:
            # Counted in steps of 100 lines, so a big file may not keep growing but a line or two is no event.
            hits.setdefault("file-too-big", {})[f] = [1] * ((len(lines) - MAX_LINES) // 100 + 1)
    return hits


def main():
    if not os.path.isdir(ROOT + "/Desert"):
        sys.exit("arch_census: no Desert/ here")
    now = scan()
    if "--list" in sys.argv:
        rule = sys.argv[sys.argv.index("--list") + 1]
        for f, ls in now.get(rule, {}).items():
            print(f"{f}: {len(ls)} {ls[:8]}" if rule != "file-too-big" else f)
        return 0
    counts = {r: {f: len(l) for f, l in sorted(d.items())} for r, d in sorted(now.items())}
    base = json.load(open(BASE, encoding="utf-8")) if os.path.exists(BASE) else None
    rises = [] if base is None else [(r, f, base.get(r, {}).get(f, 0), n, now[r][f])
                                     for r, d in counts.items() for f, n in d.items() if n > base.get(r, {}).get(f, 0)]
    total = " ".join(f"{r}={sum(d.values())}" for r, d in counts.items())
    if "--update" in sys.argv:
        if rises and "--accept-rise" not in sys.argv:
            print(f"arch_census: {len(rises)} rise(s), baseline not written")
            return 1
        with open(BASE, "w", encoding="utf-8", newline="\n") as out:
            json.dump(counts, out, indent=1, sort_keys=True)
            out.write("\n")
        print("arch_census: baseline", total)
        return 0
    if base is None:
        print("arch_census: no baseline")
        return 1
    for r, f, was, n, where in rises:
        print(f"arch_census: {r}: {f} {was} -> {n}" + ("" if r == "file-too-big" else f" lines {where[:8]}"))
    print("arch_census:", "FAILED" if rises else "ok", total)
    return 1 if rises else 0


if __name__ == "__main__":
    sys.exit(main())
