#!/usr/bin/env python3
"""Every tracked text asset parses to the same document before and after the canonical-text programme (AF6).

The canonical writer changed only LAYOUT: key order, whitespace, number spelling. A file whose parsed
document differs from the one at the base ref therefore changed CONTENT, and this names it. The only
content change AF6 made on purpose are scene v25 (AF6c): records sorted by id, each stating `siblingIndex`,
and the version integer raised; and the text header (AF6g/AF6h): scene v26, prefabs and .demat files open
with a Header stating kind, GUID and versions in place of the two integers. Scene v27 (AF7h) states each
material slot as the material's GUID text in place of the 64-bit handle; against a base that already carries
the header, that is normalised away only when the handle -> GUID pairing is one-to-one across the whole corpus
and every GUID is a .demat header's. Those are normalised away below - nothing else is.

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


_SLOT_PAIRS = {}  # old MaterialGuids handle -> new GUID text, gathered over every SCNE 26 -> 27 file


def swap_material_slots(old, new):
    """Replaces every MaterialGuids list in `old` by the one at the same place in `new` when the lengths agree,
    recording each handle -> GUID pair. False when the two documents do not line up."""
    if isinstance(old, dict) and isinstance(new, dict):
        for key, value in old.items():
            if key == "MaterialGuids" and isinstance(value, list) and isinstance(new.get(key), list):
                if len(value) != len(new[key]) or not all(isinstance(g, str) for g in new[key]):
                    return False
                for handle, guid in zip(value, new[key]):
                    _SLOT_PAIRS.setdefault(handle, set()).add(guid)
                old[key] = list(new[key])
            elif key in new and not swap_material_slots(value, new[key]):
                return False
    elif isinstance(old, list) and isinstance(new, list):
        return all(swap_material_slots(a, b) for a, b in zip(old, new))
    return True


def strip_scene_v27(old, new, ext):
    """SCNE 26 -> 27 (AF7h) against a base that already states the header: the version and the slot spelling
    are the only change. True when normalised; the pairing itself is judged once the corpus is read."""
    if ext not in (".desce", ".deprefab") or not isinstance(old, dict) or not isinstance(new, dict):
        return False
    old_v = old.get("Header", {}).get("Versions", {})
    new_v = new.get("Header", {}).get("Versions", {})
    if old_v.get("SCNE") != 26 or new_v.get("SCNE") != 27:
        return False
    old_v["SCNE"] = 27
    return swap_material_slots(old, new)


_MESH_PAIRS = {}  # old MeshGuid handle -> new GUID text, gathered over every SCNE 27 -> 28 file


def swap_mesh_guids(old, new):
    """Replaces every integer MeshGuid in `old` by the GUID text at the same place in `new`, recording each
    handle -> GUID pair. False when the two documents do not line up."""
    if isinstance(old, dict) and isinstance(new, dict):
        for key, value in old.items():
            if key == "MeshGuid" and isinstance(value, int):
                if not isinstance(new.get(key), str):
                    return False
                _MESH_PAIRS.setdefault(value, set()).add(new[key])
                old[key] = new[key]
            elif key in new and not swap_mesh_guids(value, new[key]):
                return False
    elif isinstance(old, list) and isinstance(new, list):
        return all(swap_mesh_guids(a, b) for a, b in zip(old, new))
    return True


def strip_scene_v28(old, new, ext):
    """SCNE 27 -> 28 (AF7o): the version and the MeshGuid spelling (path handle -> header GUID text) are the
    only change. The pairing is judged once the corpus is read."""
    if ext not in (".desce", ".deprefab") or not isinstance(old, dict) or not isinstance(new, dict):
        return False
    old_v = old.get("Header", {}).get("Versions", {})
    new_v = new.get("Header", {}).get("Versions", {})
    if old_v.get("SCNE") != 27 or new_v.get("SCNE") != 28:
        return False
    old_v["SCNE"] = 28
    return swap_mesh_guids(old, new)


def material_header_guids(root, files):
    guids = set()
    for path in files:
        if path.endswith(".demat"):
            with open(f"{root}/{path}", "rb") as f:
                guids.add(json.loads(f.read()).get("Header", {}).get("Guid"))
    return guids


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
        if isinstance(old, dict) and "Header" in old:
            # The base is past AF6: both sides state the header, and only AF7h's slot spelling may differ.
            strip_scene_v27(old, new, ext)
            strip_scene_v28(old, new, ext)
            if old != new:
                differ.append(path)
            compared += 1
            continue
        header_ok = strip_text_header(old, new, ext)
        stated = isinstance(new, dict) and new.get("SceneVersion", 26 if header_ok else None)
        raised = isinstance(old, dict) and isinstance(new, dict) and old.get("SceneVersion") == 24 and \
            stated in (25, 26)
        if normalise(old, ext, raised) != normalise(new, ext, raised):
            differ.append(path)
        compared += 1

    known = material_header_guids(root, files)
    for handle, guids in sorted(_SLOT_PAIRS.items(), key=lambda kv: str(kv[0])):
        if len(guids) != 1:
            differ.append(f"handle {handle} became {len(guids)} GUIDs: {sorted(guids)}")
    by_guid = {}
    for handle, guids in _SLOT_PAIRS.items():
        for guid in guids:
            by_guid.setdefault(guid, set()).add(handle)
            if guid not in known:
                differ.append(f"slot GUID {guid} (was handle {handle}) names no .demat header")
    differ += [f"GUID {g} came from {len(h)} handles" for g, h in by_guid.items() if len(h) != 1]
    mesh_by_guid = {}
    for handle, guids in _MESH_PAIRS.items():
        if len(guids) != 1:
            differ.append(f"mesh handle {handle} became {len(guids)} GUIDs: {sorted(guids)}")
        for guid in guids:
            mesh_by_guid.setdefault(guid, set()).add(handle)
    differ += [f"mesh GUID {g} came from {len(h)} handles" for g, h in mesh_by_guid.items() if len(h) != 1]

    for path in differ:
        print(f"DIFFERS {path}")
    print(f"compare_corpus: {compared} file(s) compared against {base}, {len(differ)} differ, "
          f"{fresh} new since it; {len(_SLOT_PAIRS)} slot handle(s) paired to a GUID, "
          f"{len(_MESH_PAIRS)} mesh handle(s) paired to a GUID")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
