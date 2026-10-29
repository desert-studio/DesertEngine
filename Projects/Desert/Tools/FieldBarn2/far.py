"""FIELD-LIGHT: the far ring around FIELD_Barn's 240 m field - a second landscape, 2x2 tiles of 1020 m (4 m quads),
centred on the field. Outside the field it continues terrain.height() and rises into hills that close the horizon;
inside the field it ducks 5 m under the near landscape. Its vertices land exactly on the field's border (12000 cm =
285 quads from the origin), so the two surfaces meet there. Writes the .dlht tiles and adds the far landscape, its
tiles and the scene's height fog to FIELD_Barn.desce (idempotent)."""
import sys, os, json, math, struct, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import terrain as t

TILES, QUADS, SPACING, ZSCALE = 2, 255, 400.0, 64.0
ORIGIN = -TILES * QUADS * SPACING / 2.0  # -102000
FIELD = t.TILES * t.QUADS * t.SPACING / 2.0  # 12000
LAND_ID, FOG_ID, TILE0 = 8180000000000900, 8180000000000901, 8180000000000910
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../Content"))
SCENE = os.path.join(ROOT, "Scenes/FIELD_Barn.desce")
TILE_DIR = os.path.join(ROOT, "Scenes/FIELD_Barn_Landscape")


def far_height(x, z):
    d = max(abs(x), abs(z))
    if d < FIELD - 1.0:
        return t.height(x, z) - 500.0
    # hills: nothing at the field border, rolling 20-60 m by 600 m out
    ramp = t.smoothstep(FIELD, 60000.0, d)
    hills = 2600.0 + 5200.0 * t.fbm(x, z, 52000.0, 4, 71) + 900.0 * t.fbm(x, z, 9000.0, 3, 83)
    return t.height(x, z) + ramp * max(hills, 0.0)


def sample(hcm):
    return max(0, min(65535, int(round(hcm / ZSCALE * 128.0 + 32768.0))))


def write_tile(path, tx, tz):
    n = QUADS + 1
    hs, wg, wd, wy = [], bytearray(), bytearray(), bytearray()
    for j in range(n):
        z = ORIGIN + (tz * QUADS + j) * SPACING
        for i in range(n):
            x = ORIGIN + (tx * QUADS + i) * SPACING
            hs.append(sample(far_height(x, z)))
            if max(abs(x), abs(z)) < FIELD - 1.0:
                wd.append(255); wy.append(0); wg.append(0)  # hidden under the field: no grass layer
                continue
            dry = t.smoothstep(-0.12, 0.18, t.fbm(x, z, 2500.0, 3, 23) + 0.25 * t.fbm(x, z, 400.0, 2, 29))
            dv = int(round(255 * dry))
            wd.append(0); wy.append(dv); wg.append(255 - dv)
    planes = [(b"FieldDirt", wd), (b"FieldGrass", wg), (b"FieldDry", wy)]

    def weight_section():
        out = struct.pack("<I", len(planes))
        for name, w in planes:
            out += struct.pack("<I", len(name)) + name + bytes(w)
        return out

    hb = struct.pack("<%dH" % len(hs), *hs)
    body = struct.pack("<4sIIIIQ", b"DLHT", 3, n, n, 1, len(hb)) + hb + weight_section()
    body += struct.pack("<QI", t.EDIT_LAYER_GUID, len(hs)) + hb + weight_section()
    body += struct.pack("<I", t.crc32c(body))
    with open(path, "wb") as f:
        f.write(body)


for tz in range(TILES):
    for tx in range(TILES):
        write_tile(f"{TILE_DIR}/{TILE0 + tz * TILES + tx}.dlht", tx, tz)

text = open(SCENE).read()
scene = json.loads(text)
ents = scene["Entities"]
if any(e["id"] == LAND_ID for e in ents):
    print("far ring tiles rewritten; scene already has the far ring"); sys.exit(0)
near = next(e for e in ents if e["id"] == 8180000000000005)
sib = max(e["siblingIndex"] for e in ents) + 1
I = [0.0, 0.0, 0.0]; S = [1.0, 1.0, 1.0]
land = json.loads(json.dumps(near))
land.update({"id": LAND_ID, "siblingIndex": sib, "Tag": "Terrain Far", "Translation": [ORIGIN, 0.0, ORIGIN]})
land["Landscape"].update({"QuadsPerTile": QUADS, "SpacingCm": SPACING, "ZScale": ZSCALE})
new = [land, {"id": FOG_ID, "siblingIndex": sib + 1, "Tag": "Height Fog", "Translation": I, "Rotation": I, "Scale": S,
              "ExponentialHeightFog": {
                  "Enabled": True, "FogDensity": 0.02, "FogHeightFalloff": 0.2,
                  "FogInscatteringLuminance": [0.447, 0.638, 1.0],
                  "SkyAtmosphereAmbientContributionColorScale": [1.0, 1.0, 1.0], "FogMaxOpacity": 1.0,
                  "StartDistance": 0.0, "FogCutoffDistance": 0.0, "SecondFogDensity": 0.0,
                  "SecondFogHeightFalloff": 0.2, "SecondFogHeightOffset": 0.0,
                  "DirectionalInscatteringExponent": 4.0, "DirectionalInscatteringStartDistance": 10000.0,
                  "DirectionalInscatteringLuminance": [10.0, 8.0, 5.5]}}]
for tz in range(TILES):
    for tx in range(TILES):
        tid = TILE0 + tz * TILES + tx
        new.append({"id": tid, "siblingIndex": sib + 2 + tz * TILES + tx, "Tag": f"Terrain Far Tile {tx}_{tz}",
                    "Translation": I, "Rotation": I, "Scale": S,
                    "LandscapeTile": {"Landscape": str(LAND_ID), "TileX": tx, "TileZ": tz,
                                      "HeightFile": f"assets:Scenes/FIELD_Barn_Landscape/{tid}.dlht"}})
# the scene's own layout: 4-space indent, number arrays on one line
block = ",\n".join(json.dumps(e, indent=4) for e in new)
block = re.sub(r"\[\s*([-0-9.e, \n]+?)\s*\]", lambda m: "[" + ", ".join(v.strip() for v in m.group(1).split(",")) + "]", block)
block = "\n".join("        " + l for l in block.split("\n"))
end = text.rindex("\n        }\n    ]")
text = text[:end] + "\n        },\n" + block[:-1].rstrip() + "\n" + text[end + 1:]
open(SCENE, "w").write(text)
json.loads(text)
print("far ring + fog written")
