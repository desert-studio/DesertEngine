#!/usr/bin/env python3
"""Every tracked text asset parses to the same document before and after the canonical-text programme (AF6).

The canonical writer changed only LAYOUT: key order, whitespace, number spelling. A file whose parsed
document differs from the one at the base ref therefore changed CONTENT, and this names it. The only
content change AF6 made on purpose are scene v25 (AF6c): records sorted by id, each stating `siblingIndex`,
and the version integer raised; and the text header (AF6g/AF6h): scene v26, prefabs and .demat files open
with a Header stating kind, GUID and versions in place of the two integers. Those are normalised away below
- nothing else is.

Run from anywhere inside the repository:
    python3 Desert/Tests/Common/CanonicalText/compare_corpus.py [base-ref]
The base ref defaults to origin/task/AF2-cells-envelope, the last tree before AF6. Exit 1 on any difference.
It is a script and not a gtest because the answer needs the git history, which a CI checkout at depth 1 lacks.
"""
import json
import os
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


_LEGACY_IDS = None


def legacy_material_ids():
    """The MATL 1 -> 2 register (Tools/SceneMigrator LegacyMaterialIds): old MaterialId -> GUID text."""
    global _LEGACY_IDS
    if _LEGACY_IDS is None:
        root = git("rev-parse", "--show-toplevel").decode().strip()
        with open(os.path.join(root, "Editor", "Resources", "LegacyMaterialIds.json")) as f:
            _LEGACY_IDS = {row["MaterialId"]: row["Guid"] for row in json.load(f)["Ids"]}
    return _LEGACY_IDS


def strip_material_identity(old, new, header):
    """MATL 2 (AF7c): the old file's MaterialId is gone and is the register's name for this file's GUID; its
    ParentMaterialId is now `Parent`, the register's GUID for it, stated again as the one Dependency. True when
    exactly that changed, with the identity members removed from both sides."""
    ids = legacy_material_ids()
    old_header = old.pop("Header", None)
    if old_header is not None and old_header.get("Guid") != header.get("Guid"):
        return False
    if "MaterialId" in new or "ParentMaterialId" in new:
        return False
    if "MaterialId" in old and ids.get(old.pop("MaterialId")) != header.get("Guid"):
        return False
    parent = old.pop("ParentMaterialId", None)
    if parent is None:
        return "Parent" not in new and header.get("Dependencies") == []
    guid = ids.get(parent)
    return guid is not None and new.pop("Parent", None) == guid and header.get("Dependencies") == [guid]


def strip_text_header(old, new, ext):
    """Removes the text header AF6g/AF6h added (scene v26, .demat MATL 1) when it states exactly what the old
    file did: kind by extension, SCNE 26 with the old UnitVersion carried as UNIT, MATL 1. True when stripped."""
    if not isinstance(old, dict) or not isinstance(new, dict) or "Header" not in new:
        return False
    header = new["Header"]
    versions = header.get("Versions", {})
    if ext in (".desce", ".deprefab"):
        kind = "Scene" if ext == ".desce" else "Prefab"
        if header.get("Kind") != kind or versions.get("SCNE") != 26 or "UnitVersion" in new or \
                versions.get("UNIT") != old.get("UnitVersion"):
            return False
        old.pop("UnitVersion", None)
    elif ext == ".demat":
        if header.get("Kind") != "Material" or versions not in ({"MATL": 1}, {"MATL": 2}):
            return False
        if versions == {"MATL": 2} and not strip_material_identity(old, new, header):
            return False
    else:
        return False
    new.pop("Header")
    return True


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
        header_ok = strip_text_header(old, new, ext)
        stated = isinstance(new, dict) and new.get("SceneVersion", 26 if header_ok else None)
        raised = isinstance(old, dict) and isinstance(new, dict) and old.get("SceneVersion") == 24 and \
            stated in (25, 26)
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
