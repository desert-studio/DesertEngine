"""Writes FieldBarn/Meshes/{FenceRail,HayBale}/*.gltf (metres, Y up): a rustic post-and-rail fence segment (3 m along +X,
its post at x=0; segments chain end to end) and a round hay bale (1.5 m diameter, 1.2 m wide, axis X, resting on y=0)."""
import json, struct, math, os, random

ROOT = "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-SSR/Projects/Desert/Content/Environments/FieldBarn/Meshes"
TEX = "../../Textures"


def sub(a, b): return [a[i] - b[i] for i in range(3)]
def add(a, b): return [a[i] + b[i] for i in range(3)]
def mul(a, s): return [x * s for x in a]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def dot(a, b): return sum(a[i] * b[i] for i in range(3))
def norm(a):
    l = math.sqrt(dot(a, a)); return [x / l for x in a]


def write(name, prims, mats):
    """prims: {mat: [(pos, nrm, uv)]}; mats: {mat: (texset, baseColorFactor)}."""
    bin_ = bytearray(); views, accs, prim_json = [], [], []
    names = list(prims)
    for mi, m in enumerate(names):
        vs = prims[m]; attrs = {}
        for an, idx, n in (("POSITION", 0, 3), ("NORMAL", 1, 3), ("TEXCOORD_0", 2, 2)):
            data = [v[idx] for v in vs]
            while len(bin_) % 4: bin_.append(0)
            off = len(bin_)
            for d in data: bin_ += struct.pack("<%df" % n, *d)
            views.append({"buffer": 0, "byteOffset": off, "byteLength": len(bin_) - off})
            acc = {"bufferView": len(views) - 1, "componentType": 5126, "count": len(data), "type": "VEC%d" % n}
            if an == "POSITION":
                acc["min"] = [min(p[i] for p in data) for i in range(3)]; acc["max"] = [max(p[i] for p in data) for i in range(3)]
            accs.append(acc); attrs[an] = len(accs) - 1
        prim_json.append({"attributes": attrs, "material": mi})
    images, materials = [], []

    def tex(p):
        if p not in images: images.append(p)
        return images.index(p)
    for m in names:
        t, base = mats[m]
        materials.append({"name": f"M_{name}_{m}", "pbrMetallicRoughness": {
            "baseColorFactor": base, "baseColorTexture": {"index": tex(f"{TEX}/{t}/{t}_Diffuse_2k.jpg")},
            "metallicRoughnessTexture": {"index": tex(f"{TEX}/{t}/{t}_arm_2k.jpg")}, "metallicFactor": 0.0, "roughnessFactor": 1.0},
            "normalTexture": {"index": tex(f"{TEX}/{t}/{t}_nor_gl_2k.jpg")},
            "occlusionTexture": {"index": tex(f"{TEX}/{t}/{t}_arm_2k.jpg")}})
    gl = {"asset": {"version": "2.0", "generator": "FIELD-BARN-2 props.py"}, "scene": 0, "scenes": [{"nodes": [0]}],
          "nodes": [{"name": name, "mesh": 0}], "meshes": [{"name": name, "primitives": prim_json}],
          "materials": materials, "textures": [{"source": i} for i in range(len(images))],
          "images": [{"uri": u} for u in images], "accessors": accs, "bufferViews": views,
          "buffers": [{"uri": name + ".bin", "byteLength": len(bin_)}]}
    out = os.path.join(ROOT, name); os.makedirs(out, exist_ok=True)
    open(os.path.join(out, name + ".bin"), "wb").write(bin_)
    json.dump(gl, open(os.path.join(out, name + ".gltf"), "w"), indent=1)
    print(name, {m: len(v) // 3 for m, v in prims.items()}, "tris")


def poly(out, pts, n, uaxis, vaxis, tile, uoff=0.0, voff=0.0):
    for i in range(1, len(pts) - 1):
        tri = [pts[0], pts[i], pts[i + 1]]
        if dot(cross(sub(tri[1], tri[0]), sub(tri[2], tri[0])), n) < 0:
            tri = [tri[0], tri[2], tri[1]]
        for p in tri:
            out.append((p, n, (dot(p, uaxis) / tile + uoff, -dot(p, vaxis) / tile + voff)))


def box(out, c, ax, ay, az, tile, uoff):
    """Oriented box, UV u along its longest axis so the wood grain runs the length of a rail or post."""
    longest = max((ax, ay, az), key=lambda a: dot(a, a))
    for s in (1, -1):
        for a, b, e in ((ax, ay, az), (ay, az, ax), (az, ax, ay)):
            n = norm(mul(a, s)); f = add(c, mul(a, s))
            pts = [add(add(f, mul(b, i)), mul(e, j)) for i, j in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            u = norm(longest) if abs(dot(norm(longest), n)) < 0.9 else norm(b)
            v = norm(cross(n, u))
            poly(out, pts, n, u, v, tile, uoff, uoff * 0.37)


# ── fence: one weathered post at x=0 and two sagging, slightly twisted split rails to x=3 ──
rnd = random.Random(7)
fence = []
box(fence, [0.0, 0.42, 0.0], [0.06, 0, 0], [0, 0.72, 0], [0, 0, 0.06], 1.2, 0.0)          # post, 0.3 m in the ground
box(fence, [0.0, 1.15, 0.0], [0.065, 0, 0], [0, 0.012, 0], [0, 0, 0.065], 1.2, 0.3)     # weathered cap
for k, (y0, y1) in enumerate(((0.46, 0.44), (0.88, 0.86))):
    a, b = [0.0, y0, 0.07], [3.0, y1, 0.07 + rnd.uniform(-0.02, 0.02)]
    c = mul(add(a, b), 0.5); half = mul(sub(b, a), 0.5)
    box(fence, c, half, [0, 0.045, 0], [0, 0, 0.025], 1.6, 0.23 * k)
if False: write("FenceRail", {"Wood": fence}, {"Wood": ("weathered_plank_siding", [1.05, 1.0, 0.94, 1.0])})

# ── round hay bale ──
R, HW, N = 0.75, 0.6, 40
bale = {"Side": [], "Face": []}
for i in range(N):
    a0, a1 = 2 * math.pi * i / N, 2 * math.pi * (i + 1) / N
    def ring(a, x):
        # a soft bulge: the wrap is tighter at the faces than at the middle
        r = R * (0.97 + 0.03 * (1.0 - (x / HW) ** 2))
        return [x, R + r * math.sin(a) - 0.04, r * math.cos(a)]
    xs = [-HW, -HW / 2, 0.0, HW / 2, HW]
    for j in range(len(xs) - 1):
        q = [ring(a0, xs[j]), ring(a1, xs[j]), ring(a1, xs[j + 1]), ring(a0, xs[j + 1])]
        for tri in ((0, 2, 1), (0, 3, 2)):  # counter-clockwise seen from outside
            for t in tri:
                p = q[t]; ang = a0 if t in (0, 3) else a1
                n = norm([0.0, math.sin(ang), math.cos(ang)])
                bale["Side"].append((p, n, (ang * R / 1.2, p[0] / 1.2)))
    for s in (1, -1):
        n = [float(s), 0.0, 0.0]
        tri = [[s * HW, R - 0.04, 0.0], [s * HW, R - 0.04 + R * 0.97 * math.sin(a0), R * 0.97 * math.cos(a0)],
               [s * HW, R - 0.04 + R * 0.97 * math.sin(a1), R * 0.97 * math.cos(a1)]]
        if dot(cross(sub(tri[1], tri[0]), sub(tri[2], tri[0])), n) < 0: tri = [tri[0], tri[2], tri[1]]
        for p in tri:
            # the face shows the wound layers: map radius to u so the reed stalks run round the spiral
            rr = math.hypot(p[1] - (R - 0.04), p[2]); ang = math.atan2(p[1] - (R - 0.04), p[2])
            bale["Face"].append((p, n, (rr * 2.5, ang / math.pi)))
write("HayBaleRound", bale, {"Side": ("riet_01", [1.05, 0.93, 0.66, 1.0]), "Face": ("riet_01", [0.95, 0.82, 0.55, 1.0])})
