#!/usr/bin/env python3
"""The Water_Ocean shore as a Landscape (UE: the coast is landscape the ocean laps against), and the two scenes on it.

Writes the heightmap tiles (.dlht v3) of 8 x 8 components at 50 cm, 1020 m x 1020 m, then the entities of
Scenes/Water_Ocean.desce (day, the sun 25 degrees up) and Scenes/Water_Ocean_Night.desce (the moon, the same shore).

The profile (cm, still water at 0, the sea toward -Z): a curved shoreline near z = -300; seaward a 3 % beach with a
trough at 70 m and bars at 100 m and 180 m, past 150 m an 8 % slope to the floor at -40 m; landward a 3 % beach to 40 m,
dunes to +6 m, hills to +25 m. The sides and the back fall under the water before the landscape ends, so no edge of
it is ever in sight.

Usage: python scripts/Content/water_shore_landscape.py   (after water_shore_assets.py; needs numpy)
"""
import copy, math, os, struct, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from content_files import content, crc32c, dump_text, guid_of, load_text

TILE_DIR = "Scenes/Water_Ocean_Landscape"
TILES = 8
QUADS = 255
SPACING = 50.0
ZSCALE = 16.0
ORIGIN_X = -TILES * QUADS * SPACING / 2.0
ORIGIN_Z = -58000.0
SIZE = TILES * QUADS * SPACING
FLOOR = -4000.0
LANDSCAPE_ID = 4200000000000000100
EDIT_LAYER_GUID = 4200000000000000199
LAYERS = ("ShoreSand", "ShorePebble", "ShoreRock", "ShoreGrass")
SHORE = "Environments/Shore/"


# ---- the ground ----------------------------------------------------------------------------------------------------

def lattice(ix, iz, seed):
    h = (ix.astype(np.uint64) * np.uint64(374761393) + iz.astype(np.uint64) * np.uint64(668265263) +
         np.uint64(seed * 2147483647)) & np.uint64(0xFFFFFFFF)
    h = ((h ^ (h >> np.uint64(13))) * np.uint64(1274126177)) & np.uint64(0xFFFFFFFF)
    return ((h ^ (h >> np.uint64(16))) & np.uint64(0xFFFF)).astype(np.float64) / 65535.0


def value_noise(x, z, seed):
    ix, iz = np.floor(x).astype(np.int64), np.floor(z).astype(np.int64)
    fx, fz = x - ix, z - iz
    ux, uz = fx * fx * (3.0 - 2.0 * fx), fz * fz * (3.0 - 2.0 * fz)
    a, b = lattice(ix, iz, seed), lattice(ix + 1, iz, seed)
    c, d = lattice(ix, iz + 1, seed), lattice(ix + 1, iz + 1, seed)
    return (a + (b - a) * ux) * (1.0 - uz) + (c + (d - c) * ux) * uz


def fbm(x, z, base, octaves, seed):
    """About -1..1."""
    total, amplitude, f = 0.0, 1.0, 1.0 / base
    for octave in range(octaves):
        total = total + amplitude * 2.0 * (value_noise(x * f, z * f, seed + octave) - 0.5)
        amplitude *= 0.5
        f *= 2.03
    return total


def smooth(low, high, v):
    t = np.clip((v - low) / (high - low), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def bump(v, centre, half):
    return np.exp(-((v - centre) / half) ** 2)


def smooth_min(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b + (a - b) * h - k * h * (1.0 - h)


def shoreline(x):
    return -300.0 + 500.0 * np.sin(x / 9000.0) + 120.0 * fbm(x, np.zeros_like(x), 4000.0, 2, 3)


def outcrop(x, z):
    """Where rock comes through the sand: a low headland crossing the surf to the right of the beach."""
    across = bump(x + 0.25 * z, 9000.0, 2200.0) * bump(z, 300.0, 1600.0)
    return np.clip(across * (0.75 + 0.5 * fbm(x, z, 1200.0, 3, 41)), 0.0, 1.0)


def height(x, z):
    inland = z - shoreline(x)
    sea = np.where(inland > -15000.0, inland * 0.03, -450.0 + (inland + 15000.0) * 0.08)
    # The bars break along their length, as sand bars do; the trough lies landward of the first.
    run = 0.75 + 0.35 * fbm(x, z, 7000.0, 2, 11)
    sea = sea + 140.0 * run * bump(inland, -10000.0, 1400.0) + 150.0 * run * bump(inland, -18000.0, 2000.0) \
        - 60.0 * bump(inland, -7000.0, 1300.0)
    sea = np.maximum(sea, FLOOR + 20.0)

    land = np.minimum(inland, 4000.0) * 0.03
    dune = smooth(4200.0, 6500.0, inland) * (1.0 - smooth(8500.0, 11500.0, inland))
    ridge = 0.72 + 0.28 * fbm(x + 0.4 * z, z, 2600.0, 3, 23)
    land = land + 430.0 * dune * np.clip(ridge, 0.15, 1.0) + 120.0 * smooth(9000.0, 13000.0, inland)
    hills = smooth(12000.0, 26000.0, inland)
    land = land + 2200.0 * hills * (0.8 + 0.2 * fbm(x, z, 9000.0, 4, 31))
    h = np.where(inland > 0.0, land, sea)

    h = h + 12.0 * fbm(x, z, 900.0, 3, 17) * (0.35 + 0.65 * smooth(0.0, 1200.0, np.abs(inland)))
    rock = outcrop(x, z)
    h = h + 170.0 * smooth(0.25, 0.85, rock) * (0.7 + 0.5 * fbm(x, z, 420.0, 3, 51))

    edge = np.minimum(np.minimum(x - ORIGIN_X, ORIGIN_X + SIZE - x), ORIGIN_Z + SIZE - z)
    cliff = -3700.0 + 0.4 * edge + 90.0 * fbm(x, z, 2600.0, 3, 61)
    return np.maximum(smooth_min(h, cliff, 600.0), FLOOR)


def weights(x, z, h, slope):
    """The layers' weights, uint8, summing to 255."""
    noise = fbm(x, z, 700.0, 4, 71)
    rock = np.maximum(smooth(0.42, 0.6, slope + 0.06 * noise), smooth(0.35, 0.6, outcrop(x, z) + 0.12 * noise))
    grass = smooth(240.0, 380.0, h + 90.0 * noise) * (1.0 - smooth(0.35, 0.5, slope))
    grass = grass * smooth(-0.35, 0.25, fbm(x, z, 1500.0, 3, 81) + 0.6 * smooth(700.0, 1400.0, h))
    # Shingle: a broken strip at the wash and the bars' troughs.
    pebble = smooth(-170.0, -40.0, h) * (1.0 - smooth(10.0, 60.0, h)) * smooth(0.05, 0.55, noise + 0.35)
    grass = grass * (1.0 - rock)
    pebble = pebble * (1.0 - rock) * (1.0 - grass)
    planes = np.stack([1.0 - rock - grass - pebble, pebble, rock, grass])
    out = np.floor(np.clip(planes, 0.0, 1.0) * 255.0).astype(np.int32)
    out[0] += 255 - out.sum(axis=0)
    return np.clip(out, 0, 255).astype(np.uint8)


def ground_at(x, z):
    return float(height(np.array([[float(x)]]), np.array([[float(z)]]))[0, 0])


def write_tiles():
    n = TILES * QUADS + 1
    xs, zs = np.meshgrid(ORIGIN_X + np.arange(n) * SPACING, ORIGIN_Z + np.arange(n) * SPACING)
    h = height(xs, zs)
    dz, dx = np.gradient(h, SPACING)
    slope = np.hypot(dx, dz)
    w = weights(xs, zs, h, slope)
    near = (np.abs(xs) < 20000.0) & (np.abs(zs) < 20000.0)
    print("height %.0f..%.0f cm, steepest within 200 m %.1f deg, anywhere %.1f deg, edges up to %.0f cm" % (
        h.min(), h.max(), math.degrees(math.atan(slope[near].max())), math.degrees(math.atan(slope.max())),
        max(h[0].max(), h[-1].max(), h[:, 0].max(), h[:, -1].max())))
    print("layers, share of the dry land:", {k: round(float((w[i][h > 0] / 255.0).mean()), 3)
                                             for i, k in enumerate(LAYERS)})
    stored = np.clip(np.rint(h / ZSCALE * 128.0 + 32768.0), 0, 65535).astype("<u2")
    os.makedirs(content(TILE_DIR), exist_ok=True)
    for tz in range(TILES):
        for tx in range(TILES):
            cut = np.s_[tz * QUADS:tz * QUADS + QUADS + 1, tx * QUADS:tx * QUADS + QUADS + 1]
            heights = stored[cut].tobytes()
            planes = [(name, w[i][cut]) for i, name in enumerate(LAYERS) if w[i][cut].any()]
            layers = struct.pack("<I", len(planes)) + b"".join(
                struct.pack("<I", len(name)) + name.encode() + plane.tobytes() for name, plane in planes)
            samples = QUADS + 1
            body = struct.pack("<4sIIIIQ", b"DLHT", 3, samples, samples, 1, len(heights)) + heights + layers
            body += struct.pack("<QI", EDIT_LAYER_GUID, samples * samples) + heights + layers
            body += struct.pack("<I", crc32c(body))
            with open(content("%s/%d.dlht" % (TILE_DIR, LANDSCAPE_ID + 1 + tz * TILES + tx)), "wb") as f:
                f.write(body)


# ---- the scenes ----------------------------------------------------------------------------------------------------

def entity(scene_key, tag, index, components, translation=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0), ident=None):
    if ident is None:
        ident = int(guid_of(scene_key + ":" + tag)[:16], 16) >> 1
    return {"id": ident, "siblingIndex": index, "Tag": tag, "Translation": list(translation),
            "Rotation": list(rotation), "Scale": [1.0, 1.0, 1.0], **components}


def rotation_towards(source):
    """The Euler rotation (glm: Rz Ry Rx) whose local -Z is the way the light of a source in this direction travels."""
    x, y, z = np.array(source) / np.linalg.norm(source)
    # Travel = -source = R * (0, 0, -1), so R * (0, 0, 1) = source.
    return [-math.asin(y), math.atan2(x, z), 0.0]


def layer_refs():
    refs = []
    for name in LAYERS:
        path = "Landscape/Layers/Shore/%s.delayerinfo" % name
        refs.append({"Guid": load_text(content(path))["Header"]["Guid"], "Path": "assets:" + path})
    return refs


def landscape(index, offset):
    """The Landscape and its tiles; @p offset keeps the two scenes' entity ids apart."""
    root = LANDSCAPE_ID + offset
    material = "%sMaterials/M_Shore_Landscape.demat" % SHORE
    result = [entity("", "Shore", index, {
        "Landscape": {"QuadsPerTile": QUADS, "SpacingCm": SPACING, "ZScale": ZSCALE,
                      "EditLayers": [{"Guid": str(EDIT_LAYER_GUID), "Name": "Base", "Visible": True, "Locked": False,
                                      "HeightAlpha": 1.0, "WeightAlpha": 1.0}],
                      "Layers": layer_refs()},
        "LandscapeMaterial": {"Material": {"Guid": load_text(content(material))["Header"]["Guid"],
                                           "Path": "assets:" + material}}},
        (ORIGIN_X, 0.0, ORIGIN_Z), ident=root)]
    for tz in range(TILES):
        for tx in range(TILES):
            k = 1 + tz * TILES + tx
            result.append(entity("", "Shore Tile %d_%d" % (tx, tz), index + k, {"LandscapeTile": {
                "Landscape": str(root), "TileX": tx, "TileZ": tz,
                "HeightFile": "assets:%s/%d.dlht" % (TILE_DIR, LANDSCAPE_ID + k)}}, ident=root + k))
    return result


def placed(x, z, yaw, scale, sink, roll=0.0):
    cy, sy, cr, sr = math.cos(yaw), math.sin(yaw), math.cos(roll), math.sin(roll)
    m = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]) @ np.array([[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]]) * scale
    t = (x, ground_at(x, z) - sink, z)
    return [round(float(v), 4) for v in (*m[:, 0], 0.0, *m[:, 1], 0.0, *m[:, 2], 0.0, *t, 1.0)]


def scatter(seed, count, region, wanted, scales, sink, roll=0.4):
    rng = np.random.default_rng(seed)
    result = []
    while len(result) < count:
        x, z = rng.uniform(region[0], region[1]), rng.uniform(region[2], region[3])
        if not wanted(x, z):
            continue
        scale = float(rng.uniform(*scales))
        result.append(placed(x, z, rng.uniform(0.0, 2.0 * math.pi), scale, sink * scale, rng.uniform(-roll, roll)))
    return result


def instanced(scene_key, tag, index, mesh, guid, transforms):
    return entity(scene_key, tag, index, {"InstancedStaticMesh": {
        "MeshPath": "assets:" + mesh, "MeshGuid": guid, "MaterialPaths": [], "MaterialGuids": [],
        "InstanceTransforms": transforms}})


def rocks_and_debris(scene_key, index):
    def in_outcrop(x, z):
        return float(outcrop(np.array([[x]]), np.array([[z]]))[0, 0]) > 0.3

    def in_wash(x, z):
        return -80.0 < ground_at(x, z) < 40.0

    def on_strandline(x, z):
        return 30.0 < ground_at(x, z) < 75.0

    outcrop_area = (4000.0, 13500.0, -2600.0, 2600.0)
    beach = (-9000.0, 9000.0, -3600.0, 1200.0)
    boulder = "Environments/FieldBarn/Meshes/boulder_01/boulder_01.stmesh"
    trunk = "Environments/FieldBarn/Meshes/dead_tree_trunk/dead_tree_trunk.stmesh"
    sets = [
        ("Rocks A", SHORE + "Meshes/namaqualand_boulder_03/namaqualand_boulder_03.stmesh",
         scatter(1, 34, outcrop_area, in_outcrop, (0.6, 1.7), 40.0) +
         # One stone in the wash, in front of the close cameras.
         [placed(260.0, -520.0, 0.7, 0.55, 22.0)]),
        ("Rocks B", SHORE + "Meshes/namaqualand_boulder_04/namaqualand_boulder_04.stmesh",
         scatter(2, 26, outcrop_area, in_outcrop, (0.6, 1.5), 45.0)),
        ("Rocks C", SHORE + "Meshes/namaqualand_boulder_06/namaqualand_boulder_06.stmesh",
         scatter(3, 70, outcrop_area, in_outcrop, (0.5, 1.6), 12.0) + scatter(4, 40, beach, in_wash, (0.3, 0.9), 10.0)),
        ("Rocks D", boulder, scatter(5, 16, outcrop_area, in_outcrop, (0.8, 1.8), 30.0)),
        ("Strandline Driftwood", trunk, scatter(6, 22, beach, on_strandline, (0.3, 0.9), 4.0, 0.05)),
    ]
    result = []
    for k, (tag, mesh, transforms) in enumerate(sets):
        side = content(mesh[:-len(".stmesh")] + ".gltf.deimport")
        result.append(instanced(scene_key, tag, index + k, mesh, load_text(side)["Header"]["Guid"], transforms))
    return result


def write_scene(path, guid, scene, key, offset, sun, post, fog):
    replaced = {"Sphere_20m", "Cube_60m", "Sphere_150m", "Height Fog", "Sun", "Moon", "Rocks A", "Rocks B", "Rocks C",
                "Rocks D", "Strandline Driftwood"}
    kept = [e for e in scene["Entities"]
            if e["Tag"] not in replaced and "Landscape" not in e and "LandscapeTile" not in e]
    for e in kept:
        if key:
            e["id"] = int(guid_of(key + ":" + e["Tag"])[:16], 16) >> 1
        if "WindSource" in e:
            e["WindSource"]["Direction"] = [0.1736, 0.0, 0.9848]  # toward the shore
        if "SkyAtmosphere" in e:
            e["SkyAtmosphere"].update(SkyAndAerialPerspectiveLuminanceFactor=[1.0, 1.0, 1.0],
                                      SunAngularDiameter=0.545, SunColor=[1.0, 1.0, 1.0])
        if "PostProcessVolume" in e:
            e["PostProcessVolume"]["Settings"].update(post)
    base = max(e.get("siblingIndex", 0) for e in kept) + 1
    made = [entity(key or "day", sun["Tag"], base, {"DirectionLight": sun["Light"]}, rotation=sun["Rotation"],
                   ident=sun.get("id")),
            entity(key or "day", "Height Fog", base + 1, {"ExponentialHeightFog": fog})]
    made += rocks_and_debris(key or "day", base + 2)
    made += landscape(base + 2 + 5, offset)
    scene["Entities"] = sorted(kept + made, key=lambda e: e["id"])
    scene["Header"]["Guid"] = guid

    deps = set()

    def collect(value):
        if isinstance(value, dict):
            for k, v in value.items():
                if k in ("Guid", "MeshGuid") and isinstance(v, str) and len(v) == 32:
                    deps.add(v)
                collect(v)
        elif isinstance(value, list):
            for v in value:
                collect(v)

    collect(scene["Entities"])
    scene["Header"]["Dependencies"] = sorted(deps)
    dump_text(content(path), scene)


def main():
    write_tiles()
    day_path, night_path = "Scenes/Water_Ocean.desce", "Scenes/Water_Ocean_Night.desce"
    field = load_text(content("Scenes/FIELD_Barn.desce"))
    fog = copy.deepcopy(next(e for e in field["Entities"] if "ExponentialHeightFog" in e)["ExponentialHeightFog"])
    light = copy.deepcopy(next(e for e in field["Entities"] if "DirectionLight" in e)["DirectionLight"])
    look = {"GlobalIlluminationMethod": 4, "ReflectionsMethod": 3, "ShadowsMethod": 1, "ContactShadows": True,
            "Tonemapper": 2, "AutoExposure": True, "AutoExposureKey": 0.18, "ManualExposureEV100": 0.0,
            "EnableBloom": True, "BloomThreshold": 2.5, "BloomIntensity": 0.35}

    scene = load_text(content(day_path))
    night = copy.deepcopy(scene)
    # 25 degrees up, seaward to the right: the surf seen from the beach has it behind the crests.
    sun = {"Tag": "Sun", "Light": light, "Rotation": rotation_towards((0.64, 0.4226, -0.64)), "id": 3651295826201078030}
    write_scene(day_path, scene["Header"]["Guid"], scene, "", 0, sun, look, fog)

    moon_light = copy.deepcopy(light)
    moon_light.update(Color=[0.8, 0.88, 1.0], Intensity=0.25)
    moon = {"Tag": "Moon", "Light": moon_light, "Rotation": rotation_towards((0.45, 0.5, -0.74))}
    night_fog = copy.deepcopy(fog)
    night_fog.update(FogInscatteringLuminance=[0.0, 0.0, 0.0], DirectionalInscatteringLuminance=[0.0, 0.0, 0.0])
    night["SceneName"] = "Water_Ocean_Night"
    write_scene(night_path, guid_of(night_path), night, night_path, 1000, moon,
                dict(look, AutoExposureKey=0.06), night_fog)


if __name__ == "__main__":
    main()
