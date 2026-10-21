"""FIELD-BARN-2 content pass on FIELD_Barn.desce (edits in place; idempotent: drops every entity tagged 'FB2 ' first).
- Dense meadow: ISM fields of clump grass scattered by noise-driven density (the procedural volumes floor per 30 m tile),
  three tones (lush / base / dry by the terrain's FieldDry noise) as material variants of the same plants.
- Path edge: bermuda tufts creeping onto the dirt along a noisy edge.
- Props: fence lines, round hay bales, a second tree species in the tree line, stumps, a log, boulders, barrels."""
import json, re, math, os, random, sys, hashlib, copy, functools
sys.path.insert(0, "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-scratch/FIELD-BARN-2")
import terrain as t

C = "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-SSR/Projects/Desert/Content"
SCENE = C + "/Scenes/FIELD_Barn.desce"
PROBE = C + "/Scenes/FB2_Probe.desce"
MATDIR = "Materials/Environments/FieldBarn/Meshes"
rnd = random.Random(20261019)
scene = json.load(open(SCENE))
ents = [e for e in scene["Entities"] if not e["Tag"].startswith("FB2 ")]
for i, e in enumerate(ents): e["siblingIndex"] = i


def guid(name): return hashlib.md5(("FIELD-BARN-2/" + name).encode()).hexdigest()


# ── mesh references: the plants from the existing fields, the new props from the probe import ──
MESH = {}
for e in ents:
    if "InstancedStaticMesh" in e:
        m = e["InstancedStaticMesh"]; MESH[m["MeshPath"].rsplit("/", 1)[1][:-7]] = (m["MeshPath"], m["MeshGuid"])
if os.path.exists(PROBE):
    for e in json.load(open(PROBE))["Entities"]:
        if "StaticMesh" in e and "MeshPath" in e["StaticMesh"]:
            m = e["StaticMesh"]; MESH[m["MeshPath"].rsplit("/", 1)[1][:-7]] = (m["MeshPath"], m["MeshGuid"])
FTYPE = {}
for e in ents:
    if "Foliage" in e and "InstancedStaticMesh" in e:
        FTYPE[e["InstancedStaticMesh"]["MeshPath"]] = e["Foliage"]

# ── grass material tone variants (AlbedoColor / SubsurfaceColor on a copy of the plant's material) ──
TONES = {"Lush": ([0.78, 0.95, 0.72, 1.0], [0.42, 0.62, 0.16, 1.0]),
         "Dry": ([1.32, 1.12, 0.62, 1.0], [0.75, 0.62, 0.22, 1.0])}


@functools.lru_cache(None)
def variant(plant, tone):
    base = f"{MATDIR}/{plant}/{plant}/{plant}.demat"
    if tone == "Base":
        m = json.load(open(f"{C}/{base}")); return base, m["Header"]["Guid"]
    rel = f"{MATDIR}/{plant}/{plant}/{plant}_{tone}.demat"
    m = json.load(open(f"{C}/{base}"))
    m["Header"]["Guid"] = guid(rel)
    for p in m["Params"]:
        if p["Name"] == "AlbedoColor": p["Value"] = TONES[tone][0]
        if p["Name"] == "SubsurfaceColor": p["Value"] = TONES[tone][1]
    json.dump(m, open(f"{C}/{rel}", "w"), indent=4)
    return rel, m["Header"]["Guid"]


def normal(x, z):
    e = 30.0
    dx = t.height(x + e, z) - t.height(x - e, z); dz = t.height(x, z + e) - t.height(x, z - e)
    n = [-dx, 2 * e, -dz]; l = math.sqrt(sum(v * v for v in n)); return [v / l for v in n]


def matrix(x, y, z, yaw, s, up=(0.0, 1.0, 0.0), sy=None):
    """Column-major TRS: yaw about Y, then the Y axis leaned onto `up`."""
    c, sn = math.cos(yaw), math.sin(yaw)
    X = [c, 0.0, -sn]; Z = [sn, 0.0, c]; Y = list(up)
    # re-orthogonalise X and Z against the leaned up
    d = sum(X[i] * Y[i] for i in range(3)); X = [X[i] - d * Y[i] for i in range(3)]
    lx = math.sqrt(sum(v * v for v in X)); X = [v / lx for v in X]
    Z = [X[1] * Y[2] - X[2] * Y[1], X[2] * Y[0] - X[0] * Y[2], X[0] * Y[1] - X[1] * Y[0]]
    sy = s if sy is None else sy
    return [X[0] * s, X[1] * s, X[2] * s, 0.0, Y[0] * sy, Y[1] * sy, Y[2] * sy, 0.0,
            Z[0] * s, Z[1] * s, Z[2] * s, 0.0, float(x), float(y), float(z), 1.0]  # the scene reader takes no JSON integers


fields = {}  # (mesh, material rel) -> transforms


def put(mesh, mat, xf):
    fields.setdefault((mesh, mat), []).append(xf)


def dry_w(x, z):
    return t.smoothstep(-0.12, 0.18, t.fbm(x, z, 2500.0, 3, 23) + 0.25 * t.fbm(x, z, 400.0, 2, 29))


def in_barn(x, z, pad=0.0):
    return abs(x - t.BARN[0]) < 700 + pad and abs(z - t.BARN[1]) < 1000 + pad


CAMS = [(-2600.0, 6500.0, 0.4, -0.92), (-2000.0, 4200.0, 0.3, -0.95), (300.0, 800.0, 0.33, -0.94)]


def near_cam(x, z, radius=350.0):
    # nothing taller than the ankle inside the first 3 m of a camera: it would fill the frame
    return any(math.hypot(x - cx, z - cz) < radius for cx, cz, _, _ in CAMS)


def near_boost(x, z):
    """1 inside the first 35 m of any camera's view, fading out by 60 m: the ground the frames look at closely."""
    b = 0.0
    for cx, cz, lx, lz in CAMS:
        vx, vz = x - cx, z - cz; f = vx * lx + vz * lz
        if f < -200: continue
        side = abs(vx * -lz + vz * lx)
        if side > 0.9 * max(f, 0.0) + 600: continue
        b = max(b, 1.0 - t.smoothstep(3500.0, 6000.0, f))
    return b


# clump sizes as imported (cm): medium_02_a 14x18x14, medium_02_c 28x25x27, medium_01_large 34x17x35, mid_a 24x24x24
CLUMPS = [("grass_medium_02_grass_medium_02_c", "grass_medium_02", 2.0, 2.8, 0.30),
          ("grass_medium_02_grass_medium_02_a", "grass_medium_02", 3.0, 4.0, 0.20),
          ("grass_medium_01_grass_medium_01_large_a", "grass_medium_01", 1.8, 2.6, 0.15),
          ("grass_medium_01_grass_medium_01_large_b", "grass_medium_01", 1.8, 2.6, 0.15),
          ("grass_medium_01_grass_medium_01_mid_a", "grass_medium_01", 2.0, 2.8, 0.20)]
X0, X1, Z0, Z1 = -9000.0, 7500.0, -7000.0, 11500.0
CELL = 100.0  # 1 m cells, jittered
n_grass = 0
for gz in range(int((Z1 - Z0) / CELL)):
    for gx in range(int((X1 - X0) / CELL)):
        x0, z0 = X0 + gx * CELL, Z0 + gz * CELL
        # per-m2 density: 2 everywhere, 6 near the cameras, thinned in noise-driven bare patches
        thin = t.smoothstep(-0.25, 0.05, t.fbm(x0, z0, 900.0, 2, 41))
        dens = (2.2 + 4.0 * near_boost(x0, z0)) * (0.5 + 0.5 * thin)
        k = int(dens) + (1 if rnd.random() < dens - int(dens) else 0)
        for _ in range(k):
            x, z = x0 + rnd.random() * CELL, z0 + rnd.random() * CELL
            pw = t.path_weight(x, z)
            if pw > 0.3 or in_barn(x, z) or near_cam(x, z, 150.0): continue
            r = rnd.random(); acc = 0.0
            for mesh, plant, smin, smax, p in CLUMPS:
                acc += p
                if r < acc: break
            dw = dry_w(x, z); lush = t.smoothstep(0.0, 0.25, t.fbm(x, z, 1400.0, 2, 53))
            tone = "Dry" if rnd.random() < dw * 0.85 else ("Lush" if rnd.random() < lush else "Base")
            n = normal(x, z); up = [0.15 * n[0], 0.15 * n[1] + 0.85, 0.15 * n[2]]
            l = math.sqrt(sum(v * v for v in up)); up = [v / l for v in up]
            s = rnd.uniform(smin, smax) * (0.75 if pw > 0.1 else 1.0)
            put(mesh, variant(plant, tone), matrix(x, t.height(x, z) - 1.5, z, rnd.uniform(0, 6.283), s, up))
            n_grass += 1

# ── path edge: tufts creeping onto the dirt where the path weight falls off ──
for seg in zip(t.PATH, t.PATH[1:]):
    (ax, az), (bx, bz) = seg; L = math.hypot(bx - ax, bz - az)
    for i in range(int(L / 25.0)):
        u = i * 25.0 / L; px, pz = ax + (bx - ax) * u, az + (bz - az) * u
        nx, nz = -(bz - az) / L, (bx - ax) / L
        for side in (1, -1):
            d = rnd.gauss(125.0, 30.0)
            x, z = px + side * nx * d, pz + side * nz * d
            if in_barn(x, z) or near_cam(x, z): continue
            mesh = "grass_bermuda_01_grass_bermuda_01_medium_a" if rnd.random() < 0.6 else "grass_medium_02_grass_medium_02_c"
            plant = mesh.split("_grass_")[0] if "bermuda" in mesh else "grass_medium_02"
            put(mesh, variant(plant, "Dry" if rnd.random() < 0.4 else "Base"),
                matrix(x, t.height(x, z) - 1.0, z, rnd.uniform(0, 6.283), rnd.uniform(2.4, 3.4)))

# ── props ──
def prop(mesh, x, z, yaw, s=1.0, dy=0.0, up=(0.0, 1.0, 0.0)):
    if mesh in MESH: put(mesh, None, matrix(x, t.height(x, z) + dy, z, yaw, s, up))


def fence_line(pts, seed):
    fr = random.Random(seed)
    for (ax, az), (bx, bz) in zip(pts, pts[1:]):
        L = math.hypot(bx - ax, bz - az); n = max(1, round(L / 300.0))
        for i in range(n):
            x0, z0 = ax + (bx - ax) * i / n, az + (bz - az) * i / n
            x1, z1 = ax + (bx - ax) * (i + 1) / n, az + (bz - az) * (i + 1) / n
            if fr.random() < 0.07: continue  # a missing span reads as an old fence
            yaw = math.atan2(-(z1 - z0), x1 - x0)
            sx = math.hypot(x1 - x0, z1 - z0) / 300.0
            lean = [fr.uniform(-0.04, 0.04), 1.0, fr.uniform(-0.04, 0.04)]
            l = math.sqrt(sum(v * v for v in lean)); lean = [v / l for v in lean]
            m = matrix(x0, t.height(x0, z0) - 4.0, z0, yaw, 1.0, lean)
            # stretch only along the rail (local X) so a span meets the next post
            for k in range(3): m[k] *= sx
            if "FenceRail" in MESH: put("FenceRail", None, m)


# left of the path, from the foreground up to the yard, and a paddock line across the right meadow
off = []
for (ax, az), (bx, bz) in zip(t.PATH[:-2], t.PATH[1:-1]):
    L = math.hypot(bx - ax, bz - az); nx, nz = -(bz - az) / L, (bx - ax) / L
    off.append((ax + nx * -600.0, az + nz * -600.0))
off.append((t.PATH[-2][0] - 600.0, t.PATH[-2][1] - 150.0))
fence_line(off, 3)
fence_line([(900.0, 2600.0), (3200.0, 2100.0), (5600.0, 1200.0), (7400.0, 900.0)], 5)
fence_line([(3200.0, 2100.0), (3600.0, -1200.0), (3900.0, -2400.0)], 6)

for x, z, yaw in [(2200, -900, 0.4), (2520, -650, 1.6), (2350, -1250, 2.6), (0, 3800, 0.2), (320, 3500, 1.3), (-150, 3250, 2.4), (2500, 900, 0.3), (2780, 1050, 1.1), (2350, 1300, 2.2), (4300, 300, 0.9), (4550, 500, 2.8),
                  (5100, -400, 1.7), (-3600, 2400, 0.5), (-3350, 2600, 1.9), (2700, -2100, 0.2), (2950, -2300, 1.4)]:
    prop("HayBaleRound", x, z, yaw, rnd.uniform(0.95, 1.05), -6.0)
for x, z, s in [(-6200, -8800, 2.6), (-3900, -9600, 3.0), (-1200, -8600, 2.4), (2400, -9900, 3.2), (4700, -8300, 2.7),
                (6600, -9400, 2.9), (-8200, -9900, 3.1), (500, -10800, 3.4), (-5200, 1800, 2.4), (5900, -2200, 2.2), (-600, -6000, 2.8), (4200, -5200, 2.6), (-3000, -6800, 3.0)]:
    prop("tree_small_02", x, z, rnd.uniform(0, 6.283), s, -10.0)
prop("tree_stump_01", -2050.0, 1900.0, 0.4, 1.4, -4.0)
prop("tree_stump_01", 2150.0, 300.0, 2.1, 1.2, -4.0)
prop("dead_tree_trunk", -3100.0, 3700.0, 1.2, 1.3, -8.0)
prop("boulder_01", -1300.0, 2600.0, 0.8, 0.9, -15.0)
prop("boulder_01", 1100.0, -500.0, 2.5, 0.6, -10.0)
prop("boulder_01", -4300.0, 5200.0, 1.7, 1.1, -20.0)
prop("wooden_barrels_01", 2450.0, -2450.0, 0.6, 1.0, 0.0)

# ── write the fields ──
nid = 8190000000000001
for (mesh, mat), xfs in sorted(fields.items(), key=lambda kv: (kv[0][0], str(kv[0][1]))):
    path, mg = MESH[mesh]
    ism = {"MeshPath": path, "MeshGuid": mg, "MaterialPaths": [mat[0]] if mat else [], "MaterialGuids": [mat[1]] if mat else [],
           "InstanceTransforms": xfs}
    tone = mat[0].rsplit("/", 1)[1][:-6] if mat else ""
    e = {"id": nid, "siblingIndex": len(ents), "Tag": f"FB2 {mesh} {tone}".strip(), "Translation": [0.0, 0.0, 0.0],
         "Rotation": [0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0], "InstancedStaticMesh": ism}
    if path in FTYPE and mat: e["Foliage"] = FTYPE[path]
    ents.append(e); nid += 1
scene["Entities"] = ents
# number arrays on one line: an InstanceTransform a line instead of sixteen (the scene loads in a third of the time)
txt = json.dumps(scene, indent=4)
txt = re.sub(r"\[\s*([-0-9.e+,\s]+?)\s*\]", lambda m: "[" + re.sub(r"\s+", " ", m.group(1)).replace(", ", ",") + "]", txt)
open(SCENE, "w").write(txt)
print("grass", n_grass, "fields", len(fields), {k[0][:24] + (k[1][0][-12:] if k[1] else ""): len(v) for k, v in fields.items() if len(v) < 200})
print("props available", sorted(k for k in MESH if not k.startswith("grass")))
