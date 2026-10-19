#!/usr/bin/env python3
"""Rename shader parameter identifiers from the old `u_*` spelling to the style in rename_map.json.

Why a tool and not a hand edit: several branches edit the shaders at the same time, so the rename is re-run on the
merged integration tree. The map is the reviewed list of old -> new names; the tool only applies it.

Rules the tool enforces (an error stops the run before any file is written):
  * every `u_*` token in a shader file (.shader / .glslh) must be in the map;
  * a name whose meaning differs by pass (a "scoped" entry) must have a scope that lists the shader file;
  * a new name must not already be an identifier in the file or in a file it includes, and two old names may not
    land on the same new name in one file;
  * a C++ file that names a scoped entry must be listed in exactly one of its scopes, or the entry must give a
    `default` (test fixtures that declare their own shader of the same spelling).
In C++ only tokens inside string literals and comments are touched (that is where shader names live); a `u_*`
token outside them is reported and left alone (e.g. the Winsock `u_long`).

Running it on an already renamed tree changes nothing: the new names carry no `u_` prefix.

Usage:
  python Tools/ShaderRename/shader_rename.py            apply the map
  python Tools/ShaderRename/shader_rename.py --check    report what would change, exit 1 if anything would
  python Tools/ShaderRename/shader_rename.py --propose  print map entries for unmapped shader names (style rules)
"""

import argparse
import fnmatch
import json
import re
import subprocess
import sys
from pathlib import Path

SHADER_SUFFIXES = (".shader", ".glslh")
CPP_SUFFIXES = (".cpp", ".hpp", ".h", ".inl")
EXCLUDED_PREFIXES = ("ThirdParty/", ".claude/", ".gauntlet/")
SHADER_ROOT = "Editor/Resources/Shaders"
TOKEN = re.compile(r"(?<![A-Za-z0-9_])u_[A-Za-z0-9_]+")
IDENT = re.compile(r"(?<![A-Za-z0-9_.])[A-Za-z_][A-Za-z0-9_]*")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
TEXTURE_DECL = re.compile(
    r"\b(?:u?i?image\w*|u?i?sampler(?:1D|2D|3D|Cube)\w*|u?i?texture(?:1D|2D|3D|Cube)\w*|Texture2D|Texture3D|TextureCube)"
    r"\s+(u_\w+)")
SAMPLER_DECL = re.compile(r"\bsampler(?:Shadow)?\s+(u_\w+)")


def repo_root(start):
    out = subprocess.check_output(["git", "-C", str(start), "rev-parse", "--show-toplevel"], text=True)
    return Path(out.strip())


def tracked_files(root):
    out = subprocess.check_output(["git", "-C", str(root), "ls-files"], text=True, encoding="utf-8")
    files = []
    for line in out.splitlines():
        if line.startswith(EXCLUDED_PREFIXES):
            continue
        if line.endswith(SHADER_SUFFIXES) or line.endswith(CPP_SUFFIXES):
            files.append(line)
    return files


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def cpp_literal_spans(text):
    """Spans of string literals (incl. raw strings) and comments in C++ source."""
    spans = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            spans.append((i, j))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            spans.append((i, j))
            i = j
        elif c == "R" and i + 1 < n and text[i + 1] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")
                                                                 or text[i - 1] in "uU8L"):
            k = text.find("(", i + 2)
            delim = text[i + 2:k]
            end = text.find(")" + delim + '"', k)
            j = n if end < 0 else end + len(delim) + 2
            spans.append((i, j))
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and (text[i - 1].isalnum()))):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            spans.append((i, min(j + 1, n)))
            i = j + 1
        else:
            i += 1
    return spans


class Map:
    def __init__(self, path):
        data = json.loads(read_text(path))
        self.names = data["names"]

    def resolve(self, old, rel, is_shader):
        """New name for `old` in file `rel`, or (None, reason)."""
        entry = self.names.get(old)
        if entry is None:
            return None, "unmapped"
        if isinstance(entry, str):
            return entry, None
        hits = [s["new"] for s in entry["scopes"] if any(fnmatch.fnmatchcase(rel, p) for p in s["files"])]
        if len(hits) == 1:
            return hits[0], None
        if len(hits) > 1:
            return None, "file listed in several scopes: " + ", ".join(hits)
        if not is_shader and "default" in entry:
            return entry["default"], None
        return None, "scoped name, file in no scope"


def word_split(name):
    return re.findall(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|[0-9]+", name)


WORDS = {"Tex": "Texture", "Pos": "Position", "Proj": "Projection", "Inv": "Inverse", "Env": "Environment",
         "Dt": "DeltaTime", "Pct": "Percent", "Src": "Source", "Dir": "Direction", "Avg": "Average",
         "Lum": "Luminance", "Attr": "Attribute", "Alloc": "Allocation", "DF": "DistanceField",
         "JFA": "JumpFlood", "BRDFLUT": "BrdfLut"}


def propose(old, kind):
    words = [WORDS.get(w, w) for w in word_split(old[2:])]
    words = [w if not (w.isupper() and len(w) > 1) else w.capitalize() for w in words]
    new = "".join(words)
    if kind == "texture" and not new.endswith("Texture"):
        new += "Texture"
    if kind == "sampler" and not new.endswith("Sampler"):
        new += "Sampler"
    return new


def strip_comments(text):
    """Shader text without comments, strings (DSL display names) and #include paths: none of them declares an
    identifier that can clash."""
    out, last = [], 0
    for a, b in cpp_literal_spans(text):
        out.append(text[last:a])
        last = b
    out.append(text[last:])
    return INCLUDE.sub("", "".join(out))


def include_closure(rel, texts, root):
    seen, stack = set(), [rel]
    while stack:
        cur = stack.pop()
        if cur in seen or cur not in texts:
            continue
        seen.add(cur)
        for inc in INCLUDE.findall(texts[cur]):
            for cand in (f"{SHADER_ROOT}/{inc}", str(Path(cur).parent / inc).replace("\\", "/")):
                if cand in texts:
                    stack.append(cand)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--propose", action="store_true")
    ap.add_argument("--map", default=str(Path(__file__).with_name("rename_map.json")))
    args = ap.parse_args()

    root = repo_root(Path(__file__).parent)
    mp = Map(args.map)
    files = tracked_files(root)
    texts = {rel: read_text(root / rel) for rel in files}
    shaders = [r for r in files if r.endswith(SHADER_SUFFIXES)]

    errors, notes, plans = [], [], {}

    if args.propose:
        kinds = {}
        for rel in shaders:
            for m in TEXTURE_DECL.finditer(texts[rel]):
                kinds[m.group(1)] = "texture"
            for m in SAMPLER_DECL.finditer(texts[rel]):
                kinds[m.group(1)] = "sampler"
        unmapped = sorted({t for rel in shaders for t in TOKEN.findall(texts[rel]) if t not in mp.names})
        for old in unmapped:
            print(f'    "{old}": "{propose(old, kinds.get(old))}",')
        return 0

    for rel in files:
        text = texts[rel]
        is_shader = rel.endswith(SHADER_SUFFIXES)
        spans = None if is_shader else cpp_literal_spans(text)
        edits = []
        for m in TOKEN.finditer(text):
            old = m.group(0)
            if spans is not None:
                inside = any(a <= m.start() < b for a, b in spans)
                if not inside:
                    if old in mp.names:
                        notes.append(f"{rel}: {old} outside a string/comment left alone")
                    continue
                if old not in mp.names:
                    continue
            new, why = mp.resolve(old, rel, is_shader)
            if new is None:
                line = text.count("\n", 0, m.start()) + 1
                errors.append(f"{rel}:{line}: {old}: {why}")
                continue
            edits.append((m.start(), m.end(), old, new))
        if edits:
            plans[rel] = edits

    # Collisions: only shader files carry identifiers that can clash.
    for rel in shaders:
        edits = plans.get(rel)
        if not edits:
            continue
        closure = include_closure(rel, texts, root)
        existing = set()
        for f in closure:
            existing.update(IDENT.findall(strip_comments(texts[f])))
        by_new = {}
        for f in closure:
            for _, _, old, new in plans.get(f, []):
                by_new.setdefault(new, set()).add(old)
        for new, olds in by_new.items():
            if len(olds) > 1:
                errors.append(f"{rel}: {', '.join(sorted(olds))} all become {new}")
            if new in existing:
                errors.append(f"{rel}: new name {new} (from {', '.join(sorted(olds))}) is already an identifier here")

    for n in sorted(set(notes)):
        print("note:", n)
    if errors:
        for e in sorted(set(errors)):
            print("error:", e)
        print(f"{len(set(errors))} error(s); nothing written")
        return 2

    changed = sum(len(e) for e in plans.values())
    if args.check:
        for rel, edits in sorted(plans.items()):
            print(f"{rel}: {len(edits)}")
        print(f"{changed} token(s) in {len(plans)} file(s) would change")
        return 1 if plans else 0

    for rel, edits in plans.items():
        text = texts[rel]
        out, last = [], 0
        for a, b, _, new in edits:
            out.append(text[last:a])
            out.append(new)
            last = b
        out.append(text[last:])
        write_text(root / rel, "".join(out))
    print(f"renamed {changed} token(s) in {len(plans)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
