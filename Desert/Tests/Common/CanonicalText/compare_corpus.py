#!/usr/bin/env python3
"""Every tracked text asset parses to the same document before and after the canonical-text programme (AF6).

The canonical writer changed only LAYOUT: key order, whitespace, number spelling. A file whose parsed
document differs from the one at the base ref therefore changed CONTENT, and this names it. The only
content change AF6 made on purpose are scene v25 (AF6c): records sorted by id, each stating `siblingIndex`,
and the version integer raised; and the text header (AF6g/AF6h): scene v26, prefabs and .demat files open
with a Header stating kind, GUID and versions in place of the two integers. Scene v27 (AF7h) states each
material slot as the material's GUID text in place of the 64-bit handle; against a base that already carries
the header, that is normalised away only when the handle -> GUID pairing is one-to-one across the whole corpus
and every GUID is a .demat header's. MATL 2 -> 3 (T6c3) replaces each Textures slot number by a GUID and a
locator, routed by slot name into Textures / CloudAssets / ShaderRefs, states those GUIDs as Dependencies, and
re-spells Params through MaterialData's float storage; that is normalised away only when every slot number pairs
one-to-one with a (GUID, locator) across the corpus and every locator names a tracked file whose text header, if
it has one, states that GUID. .decloudtype format 3 -> CLTY 4 (AF7v), .destrings 1 -> STRT 2 and .detheme 1 -> UITH 2 (T7b),
.derig 1 -> CRIG 2 and .retarget 1 -> RTGT 2 (T7c) swap FormatVersion for the header alone; .danimgraph 0 -> ANGR 1
(T7d) gains the header and had no version member to drop.
Scene v29 (T6d) spells each SkyboxHandle as {Guid, Path}; normalised away only when Path is the old key and
each key pairs one-to-one with a GUID. Scene v30 (T6f) does the same to the UI sprite and splash keys.
Those are normalised away below - nothing else is.

Run from anywhere inside the repository:
    python3 Desert/Tests/Common/CanonicalText/compare_corpus.py [base-ref]
The base ref defaults to origin/task/AF2-cells-envelope, the last tree before AF6. Exit 1 on any difference.
It is a script and not a gtest because the answer needs the git history, which a CI checkout at depth 1 lacks.
"""
import json
import os
import struct
import subprocess
import sys

TEXT_EXTENSIONS = (".desce", ".deprefab", ".demat", ".anim", ".danimgraph", ".dgraph", ".decloudtype",
                   ".destrings", ".detheme", ".derig", ".retarget", ".skeleton")
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


_SKY_PAIRS = {}  # old SkyboxHandle key -> {GUID text}, gathered over every SCNE 28 -> 29 file


def swap_guid_refs(old, new, keys, pairs):
    """Replaces every string under a key in `keys` in `old` by the {Guid, Path} object at the same place in
    `new`, provided the object's Path is that string (an empty one pairs with an empty GUID), and records the
    pairing in `pairs`. False when the two documents do not line up."""
    if isinstance(old, dict) and isinstance(new, dict):
        for key, value in old.items():
            if key in keys and isinstance(value, str):
                ref = new.get(key)
                if not isinstance(ref, dict) or set(ref) != {"Guid", "Path"} or ref["Path"] != value or \
                        (value == "") != (ref["Guid"] == ""):
                    return False
                if value:
                    pairs.setdefault(value, set()).add(ref["Guid"])
                old[key] = ref
            elif key in new and not swap_guid_refs(value, new[key], keys, pairs):
                return False
    elif isinstance(old, list) and isinstance(new, list):
        return all(swap_guid_refs(a, b, keys, pairs) for a, b in zip(old, new))
    return True


def swap_skybox_refs(old, new):
    return swap_guid_refs(old, new, {"SkyboxHandle"}, _SKY_PAIRS)


def strip_scene_v29(old, new, ext):
    """SCNE 28 -> 29 (T6d): the version and the SkyboxHandle spelling (key string -> {Guid, Path}) are the only
    change. The pairing is judged once the corpus is read."""
    if ext not in (".desce", ".deprefab") or not isinstance(old, dict) or not isinstance(new, dict):
        return False
    old_v = old.get("Header", {}).get("Versions", {})
    new_v = new.get("Header", {}).get("Versions", {})
    if old_v.get("SCNE") != 28 or new_v.get("SCNE") != 29:
        return False
    old_v["SCNE"] = 29
    return swap_skybox_refs(old, new)


_REF_PAIRS = {}  # MATL 2 slot number -> {(GUID text, locator)}, gathered over every MATL 2 -> 3 file
_MATL3_LISTS = ("Textures", "CloudAssets", "ShaderRefs")


_SPRITE_PAIRS = {}  # old sprite / splash key -> {GUID text}, gathered over every SCNE 29 -> 30 file
_SPRITE_KEYS = {"Sprite", "HoverSprite", "PressedSprite", "SplashSprite"}


def strip_scene_v30(old, new, ext):
    """SCNE 29 -> 30 (T6f): the version and the UI sprite / splash spelling (key string -> {Guid, Path}) are
    the only change. The pairing is judged once the corpus is read."""
    if ext not in (".desce", ".deprefab") or not isinstance(old, dict) or not isinstance(new, dict):
        return False
    old_v = old.get("Header", {}).get("Versions", {})
    new_v = new.get("Header", {}).get("Versions", {})
    if old_v.get("SCNE") != 29 or new_v.get("SCNE") != 30:
        return False
    old_v["SCNE"] = 30
    return swap_guid_refs(old, new, _SPRITE_KEYS, _SPRITE_PAIRS)


def as_float32(value):
    """`value` with every float rounded to the float32 MaterialData stores it as."""
    if isinstance(value, float):
        return struct.unpack("f", struct.pack("f", value))[0]
    if isinstance(value, list):
        return [as_float32(v) for v in value]
    if isinstance(value, dict):
        return {k: as_float32(v) for k, v in value.items()}
    return value


def strip_material_v3(old, new, ext):
    """MATL 2 -> 3 (T6c3): every old Textures slot {Name, TextureHandle} is the new slot of the same name in
    exactly one of Textures / CloudAssets / ShaderRefs, in the old order; number 0 is the empty slot (empty GUID
    and locator); the Dependencies are the old ones plus every slot GUID; Params differ only past float32. True
    when normalised; the pairing itself is judged once the corpus is read."""
    if ext != ".demat" or not isinstance(old, dict) or not isinstance(new, dict):
        return False
    old_h, new_h = old.get("Header", {}), new.get("Header", {})
    if old_h.get("Versions") != {"MATL": 2} or new_h.get("Versions") != {"MATL": 3}:
        return False
    if any(key in old for key in _MATL3_LISTS[1:]) or not all(isinstance(new.get(k), list) for k in _MATL3_LISTS):
        return False
    old_slots = old.get("Textures", [])
    slots = {}
    for key in _MATL3_LISTS:
        names = [ref.get("Name") for ref in new[key]]
        if names != [s.get("Name") for s in old_slots if s.get("Name") in names]:
            return False  # a slot missing from the old list, or moved
        for ref in new[key]:
            if ref.get("Name") in slots or set(ref) != {"Name", "Guid", "Path"}:
                return False
            slots[ref["Name"]] = ref
    if len(slots) != len(old_slots):
        return False
    guids = []
    for slot in old_slots:
        ref = slots[slot.get("Name")]
        if set(slot) != {"Name", "TextureHandle"}:
            return False
        if slot["TextureHandle"] == 0:
            if ref["Guid"] or ref["Path"]:
                return False
            continue
        if not ref["Guid"] or not ref["Path"]:
            return False
        _REF_PAIRS.setdefault(slot["TextureHandle"], set()).add((ref["Guid"], ref["Path"]))
        guids.append(ref["Guid"])
    deps = new_h.get("Dependencies", [])
    if len(deps) != len(set(deps)) or set(deps) != set(old_h.get("Dependencies", [])) | set(guids):
        return False
    old_h["Versions"], old_h["Dependencies"] = {"MATL": 3}, deps
    for key in _MATL3_LISTS:
        old[key] = new[key]
    for doc in (old, new):
        doc["Params"] = as_float32(doc.get("Params"))
    return True


_TEXT_HEADER_GUIDS = {}  # text header GUID -> paths, gathered over every "the file gains a header" raise

# The text kinds whose old top-level FormatVersion became the header (SceneMigrator's kTextHeaderRaises):
# extension -> (Kind, tag, old version, new version, whether an absent FormatVersion meant the old version).
TEXT_HEADER_RAISES = {
    ".decloudtype": ("CloudType", "CLTY", 3, 4, False),  # AF7v
    ".destrings": ("StringTable", "STRT", 1, 2, True),  # T7b
    ".detheme": ("UITheme", "UITH", 1, 2, True),  # T7b
    ".derig": ("ControlRig", "CRIG", 1, 2, True),  # T7c
    ".retarget": ("Retarget", "RTGT", 1, 2, True),  # T7c
    ".danimgraph": ("AnimGraph", "ANGR", 0, 1, True),  # T7d: generation 0 stated no version at all
}


def strip_text_kind_header(old, new, ext, path):
    """A text kind's FormatVersion N -> header (tag N+1): the old FormatVersion is gone and the header states
    the kind, the tag at the new version and no Dependencies - nothing else. True when stripped; GUID
    uniqueness is judged over the corpus."""
    row = TEXT_HEADER_RAISES.get(ext)
    if row is None or not isinstance(old, dict) or not isinstance(new, dict) or "Header" in old:
        return False
    kind, tag, before, after, absent_is_before = row
    header = new.get("Header", {})
    stated = old.get("FormatVersion", before if absent_is_before else None)
    if stated != before or "FormatVersion" in new or header.get("Kind") != kind or \
            header.get("Versions") != {tag: after} or header.get("Dependencies") != [] or not header.get("Guid"):
        return False
    _TEXT_HEADER_GUIDS.setdefault(header["Guid"], []).append(path)
    old.pop("FormatVersion", None)
    new.pop("Header")
    return True


def locator_file(root, locator):
    """The tracked file an `assets:` / `engine:` locator names."""
    scheme, _, rel = locator.partition(":")
    base = {"assets": "Editor/Resources/Assets", "engine": "Editor/Resources"}.get(scheme)
    return None if base is None or not rel else f"{root}/{base}/{rel}"


def locator_header_guid(file):
    """The GUID a text asset's header states; None for a file without a JSON header (a binary .detex)."""
    with open(file, "rb") as f:
        head = f.read(1)
        if head != b"{":
            return None
        doc = json.loads(head + f.read())
    return doc.get("Header", {}).get("Guid") if isinstance(doc, dict) else None


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
            strip_scene_v29(old, new, ext)
            strip_scene_v30(old, new, ext)
            strip_material_v3(old, new, ext)
            if old != new:
                differ.append(path)
            compared += 1
            continue
        strip_text_kind_header(old, new, ext, path)
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
    for what, pairs in (("skybox", _SKY_PAIRS), ("sprite", _SPRITE_PAIRS)):
        by_guid = {}
        for key, guids in pairs.items():
            if len(guids) != 1:
                differ.append(f"{what} key {key} became {len(guids)} GUIDs: {sorted(guids)}")
            for guid in guids:
                by_guid.setdefault(guid, set()).add(key)
            file = locator_file(root, key)
            if file is None or not os.path.isfile(file):
                differ.append(f"{what} key {key}: names no file")
        differ += [f"{what} GUID {g} came from {len(k)} keys" for g, k in by_guid.items() if len(k) != 1]
    ref_by_guid, ref_by_locator = {}, {}
    for handle, refs in sorted(_REF_PAIRS.items()):
        if len(refs) != 1:
            differ.append(f"material slot number {handle} became {len(refs)} refs: {sorted(refs)}")
        for guid, locator in refs:
            ref_by_guid.setdefault(guid, set()).add(handle)
            ref_by_locator.setdefault(locator, set()).add(guid)
            file = locator_file(root, locator)
            if file is None or not os.path.isfile(file):
                differ.append(f"material slot number {handle} -> {locator}: names no file")
            elif locator_header_guid(file) not in (None, guid):
                differ.append(f"material slot number {handle} -> {locator}: its header states another GUID "
                              f"than {guid}")
    differ += [f"material ref GUID {g} came from {len(h)} slot numbers" for g, h in ref_by_guid.items()
               if len(h) != 1]
    differ += [f"material ref locator {loc} named by {len(g)} GUIDs: {sorted(g)}"
               for loc, g in ref_by_locator.items() if len(g) != 1]
    differ += [f"text header GUID {g} stated by {len(p)} files: {p}" for g, p in _TEXT_HEADER_GUIDS.items()
               if len(p) != 1]

    for path in differ:
        print(f"DIFFERS {path}")
    print(f"compare_corpus: {compared} file(s) compared against {base}, {len(differ)} differ, "
          f"{fresh} new since it; {len(_SLOT_PAIRS)} slot handle(s) paired to a GUID, "
          f"{len(_MESH_PAIRS)} mesh handle(s) paired to a GUID, "
          f"{len(_SKY_PAIRS)} skybox key(s) and {len(_SPRITE_PAIRS)} sprite key(s) paired to a GUID, {len(_REF_PAIRS)} material slot number(s) "
          f"paired to a GUID and locator, {len(_TEXT_HEADER_GUIDS)} text header(s) gained")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
