#!/usr/bin/env python3
"""WATER p6: the Water_Ocean shore as a Landscape (UE: the coast is landscape the ocean laps against).

Writes the heightmap tiles (.dlht v3, one weight plane ShoreSand) and puts the Landscape + its tiles into
Content/Scenes/Water_Ocean.desce in place of any earlier seabed. The beach profile (cm, world Y; still water at 0):
a curved shoreline near z = -300, a 5 % dry beach landward, a 4.5 % shoal seaward to ~1.1 m, then a 12 % drop
to the 9 m floor - the gentle shoal is where ShoreSwell's waves outgrow the depth and break (Pass_SingleLayerWater
WaterSurfAmount). Sand ripples and swells of a few cm keep it from reading as a ramp.

Usage: python3 scripts/Content/water_shore_landscape.py   (from the tree root)
"""
import json, math, os, struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Projects", "Desert", "Content")
SCENE = os.path.join(ROOT, "Scenes", "Water_Ocean.desce")
TILE_DIR = "Scenes/Water_Ocean_Landscape"
TILES_X, TILES_Z = 2, 2
QUADS = 255
SPACING = 25.0
ZSCALE = 4.0
ORIGIN_X = -TILES_X * QUADS * SPACING / 2.0
ORIGIN_Z = -11000.0
LANDSCAPE_ID = 4200000000000000100
EDIT_LAYER_GUID = 4200000000000000199
LAYER_INFO = {"Guid": "c730e67be3da4598b852ba1021a94692", "Path": "assets:Landscape/Layers/Shore/ShoreSand.delayerinfo"}
MATERIAL = {"Guid": "95cdc28154e34f40b1ff8399d707b5b8", "Path": "assets:Environments/Shore/Materials/M_Shore_Landscape.demat"}
OLD_SEABED_DEPS = {"8254b0475d555accf00b0c7a929c73e4", "615ae6cde51242b39947fd6224df80b6"}


def _hash(ix, iz, seed):
    h = (ix * 374761393 + iz * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def _vnoise(x, z, seed):
    ix, iz = math.floor(x), math.floor(z)
    fx, fz = x - ix, z - iz
    ux, uz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
    a, b = _hash(ix, iz, seed), _hash(ix + 1, iz, seed)
    c, d = _hash(ix, iz + 1, seed), _hash(ix + 1, iz + 1, seed)
    return (a + (b - a) * ux) * (1 - uz) + (c + (d - c) * ux) * uz


def fbm(x, z, base, octaves, seed):
    s, amp, f = 0.0, 1.0, 1.0 / base
    for o in range(octaves):
        s += amp * (_vnoise(x * f, z * f, seed + o) - 0.5)
        amp *= 0.5
        f *= 2.03
    return s


def height(x, z):
    shore = -300.0 + 150.0 * math.sin(x / 2300.0) + 120.0 * fbm(x, z, 4000.0, 2, 3)
    dz = z - shore  # > 0 landward
    if dz >= 0.0:
        h = min(dz * 0.05, 140.0)
    elif dz > -2500.0:
        h = dz * 0.045
    else:
        h = max(-112.5 + (dz + 2500.0) * 0.12, -900.0)
    h += 14.0 * fbm(x, z, 900.0, 3, 17)                          # swells of the sand
    h += 2.5 * math.sin((x * 0.3 + z) / 18.0) * (1.0 if dz < 0.0 else 0.4)  # wave-built sand ripples
    return h


_T = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
    _T.append(c)


def crc32c(b):
    c = 0xFFFFFFFF
    for x in b:
        c = _T[(c ^ x) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def write_tile(path, tx, tz):
    n = QUADS + 1
    hs = []
    for j in range(n):
        z = ORIGIN_Z + (tz * QUADS + j) * SPACING
        for i in range(n):
            x = ORIGIN_X + (tx * QUADS + i) * SPACING
            hs.append(max(0, min(65535, int(round(height(x, z) / ZSCALE * 128.0 + 32768.0)))))
    weights = struct.pack("<I", 1) + struct.pack("<I", len(b"ShoreSand")) + b"ShoreSand" + bytes([255]) * (n * n)
    hb = struct.pack("<%dH" % len(hs), *hs)
    body = struct.pack("<4sIIIIQ", b"DLHT", 3, n, n, 1, len(hb)) + hb + weights
    body += struct.pack("<QI", EDIT_LAYER_GUID, len(hs)) + hb + weights
    body += struct.pack("<I", crc32c(body))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(body)


def main():
    with open(SCENE) as f:
        scene = json.load(f)
    ents = [e for e in scene["Entities"]
            if e.get("Tag") != "Seabed" and "Landscape" not in e and "LandscapeTile" not in e]
    base = max(e.get("siblingIndex", 0) for e in ents) + 1
    ents.append({
        "id": LANDSCAPE_ID, "siblingIndex": base, "Tag": "Shore",
        "Translation": [ORIGIN_X, 0.0, ORIGIN_Z], "Rotation": [0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0],
        "Landscape": {
            "QuadsPerTile": QUADS, "SpacingCm": SPACING, "ZScale": ZSCALE,
            "EditLayers": [{"Guid": str(EDIT_LAYER_GUID), "Name": "Base", "Visible": True, "Locked": False,
                            "HeightAlpha": 1.0, "WeightAlpha": 1.0}],
            "Layers": [LAYER_INFO],
        },
        "LandscapeMaterial": {"Material": MATERIAL},
    })
    k = 0
    for tz in range(TILES_Z):
        for tx in range(TILES_X):
            k += 1
            tid = LANDSCAPE_ID + k
            write_tile(os.path.join(ROOT, TILE_DIR, "%d.dlht" % tid), tx, tz)
            ents.append({
                "id": tid, "siblingIndex": base + k, "Tag": "Shore Tile %d_%d" % (tx, tz),
                "Translation": [0.0, 0.0, 0.0], "Rotation": [0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0],
                "LandscapeTile": {"Landscape": str(LANDSCAPE_ID), "TileX": tx, "TileZ": tz,
                                  "HeightFile": "assets:%s/%d.dlht" % (TILE_DIR, tid)},
            })
    scene["Entities"] = ents
    deps = [d for d in scene["Header"]["Dependencies"] if d not in OLD_SEABED_DEPS]
    for g in (LAYER_INFO["Guid"], MATERIAL["Guid"]):
        if g not in deps:
            deps.append(g)
    scene["Header"]["Dependencies"] = sorted(deps)
    with open(SCENE, "w") as f:
        json.dump(scene, f, indent=4)
        f.write("\n")


if __name__ == "__main__":
    main()
