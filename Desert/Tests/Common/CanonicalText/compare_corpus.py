#!/usr/bin/env python3
"""Every tracked text asset parses to the same document before and after the canonical-text programme (AF6).

The canonical writer changed only LAYOUT: key order, whitespace, number spelling. A file whose parsed
document differs from the one at the base ref therefore changed CONTENT, and this names it. The only
content change AF6 made on purpose is scene v25 (AF6c): records sorted by id, each stating `siblingIndex`,
and the version integer raised. Those three are normalised away below - nothing else is.

Run from anywhere inside the repository:
    python3 Desert/Tests/Common/CanonicalText/compare_corpus.py [base-ref]
The base ref defaults to origin/task/AF2-cells-envelope, the last tree before AF6. Exit 1 on any difference.
It is a script and not a gtest because the answer needs the git history, which a CI checkout at depth 1 lacks.
"""
import json
import subprocess
import sys

TEXT_EXTENSIONS = (".desce", ".deprefab", ".demat", ".anim", ".danimgraph", ".dgraph", ".decloudtype",
                   ".destrings", ".detheme", ".skeleton")
SCENE_V25 = {"SceneVersion": 25}  # the version AF6c raised scenes and prefabs to


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def normalise(doc, ext, raised):
    """Removes what scene v25 changed on purpose, and only from the files that carry it."""
    if ext not in (".desce", ".deprefab") or not isinstance(doc, dict):
        return doc
    if raised:
        for key in SCENE_V25:
            doc.pop(key, None)
    for key in ("Entities", "entities"):
        records = doc.get(key)
        if isinstance(records, list):
            for record in records:
                if isinstance(record, dict):
                    record.pop("siblingIndex", None)
            records.sort(key=lambda r: json.dumps(r, sort_keys=True))
    return doc


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "origin/task/AF2-cells-envelope"
    root = git("rev-parse", "--show-toplevel").decode().strip()
    files = [f for f in git("-C", root, "ls-files").decode().splitlines() if f.endswith(TEXT_EXTENSIONS)]
    at_base = set(git("-C", root, "ls-tree", "-r", "--name-only", base).decode().splitlines())

    compared, differ, fresh = 0, [], 0
    for path in files:
        if path not in at_base:
            fresh += 1
            continue
        ext = path[path.rfind("."):]
        old = json.loads(git("-C", root, "show", f"{base}:{path}"))
        with open(f"{root}/{path}", "rb") as f:
            new = json.loads(f.read())
        raised = isinstance(old, dict) and isinstance(new, dict) and old.get("SceneVersion") == 24 and \
            new.get("SceneVersion") == 25
        if normalise(old, ext, raised) != normalise(new, ext, raised):
            differ.append(path)
        compared += 1

    for path in differ:
        print(f"DIFFERS {path}")
    print(f"compare_corpus: {compared} file(s) compared against {base}, {len(differ)} differ, "
          f"{fresh} new since it")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
