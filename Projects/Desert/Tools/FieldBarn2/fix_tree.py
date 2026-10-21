"""tree_small_02's leaves: FoliageSurface + the Poly Haven alpha map as the opacity mask, edited in place (FIELD-BARN
fix_materials.py's rule; glTF states BLEND on an alpha-less JPG and never names the alpha map)."""
import json, struct
C = "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-SSR/Projects/Desert/Content"
FOLIAGE = ("acb8d06bbe1a49dc95c2f5ebdddb4c33", "engine:Shaders/Programs/Surface/FoliageSurface.shader")


def detex_guid(rel):
    b = open(f"{C}/{rel}", "rb").read(0x20); assert b[:4] == b"DAST", rel
    a, c = struct.unpack_from("<QQ", b, 0x10); return "%016x%016x" % (a, c)


rel = "Materials/Environments/FieldBarn/Meshes/tree_small_02/tree_small_02/tree_small_02_leaves.demat"
m = json.load(open(f"{C}/{rel}"))
tex = "Textures/FieldBarn/tree_small_02_leaves_alpha_2k.detex"
m["Shader"] = {"Guid": FOLIAGE[0], "Path": FOLIAGE[1]}
m["Textures"] = [t for t in m["Textures"] if t["Name"] != "u_OpacityTexture"] + \
    [{"Name": "u_OpacityTexture", "Guid": detex_guid(tex), "Path": "assets:" + tex}]
m["Header"]["Dependencies"] = list(dict.fromkeys([FOLIAGE[0]] + [t["Guid"] for t in m["Textures"]]))
want = {"OpacityMaskClipValue": [0.2, 0, 0, 0], "OpacityChannel": [0.0, 0, 0, 0], "SubsurfaceColor": [0.45, 0.55, 0.15, 1.0],
        "AlbedoColor": [0.9, 1.0, 0.85, 1.0]}
for p in m["Params"]:
    if p["Name"] in want: p["Value"] = want.pop(p["Name"])
m["Params"] += [{"Name": k, "Value": v} for k, v in want.items()]
m["TwoSided"] = True
json.dump(m, open(f"{C}/{rel}", "w"), indent=4)
print("ok", rel)
