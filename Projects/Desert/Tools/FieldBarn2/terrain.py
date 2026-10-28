"""FIELD-BARN terrain: heights (cm) and the dirt-path mask as functions of world X/Z (cm), plus the .dlht writer."""
import math, struct, os

TILES = 4
QUADS = 255
SPACING = 6000.0 / 255.0  # 60 m tiles
ZSCALE = 4.0
ORIGIN = -TILES * QUADS * SPACING / 2.0  # landscape entity translation (x and z)
EDIT_LAYER_GUID = 9182736455463728191

BARN = (1800.0, -3500.0)  # barn centre on the rise
BARN_PAD = 1300.0

# path polyline from the foreground to the barn door (door faces +Z)
PATH = [(-2500.0, 11000.0), (-2600.0, 7000.0), (-1500.0, 3500.0), (300.0, 600.0), (1500.0, -1200.0), (1800.0, -2300.0)]


def _hash(ix, iz, seed):
    h = (ix * 374761393 + iz * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def _vnoise(x, z, seed):
    ix, iz = math.floor(x), math.floor(z)
    fx, fz = x - ix, z - iz
    ux, uz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
    a = _hash(ix, iz, seed); b = _hash(ix + 1, iz, seed)
    c = _hash(ix, iz + 1, seed); d = _hash(ix + 1, iz + 1, seed)
    return (a + (b - a) * ux) * (1 - uz) + (c + (d - c) * ux) * uz


def fbm(x, z, base, octaves, seed):
    s, amp, f = 0.0, 1.0, 1.0 / base
    for o in range(octaves):
        s += amp * (_vnoise(x * f, z * f, seed + o) - 0.5)
        amp *= 0.5; f *= 2.03
    return s


def smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def path_distance(x, z):
    best = 1e30
    for (ax, az), (bx, bz) in zip(PATH, PATH[1:]):
        dx, dz = bx - ax, bz - az
        t = max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / (dx * dx + dz * dz)))
        px, pz = ax + t * dx, az + t * dz
        best = min(best, math.hypot(x - px, z - pz))
    return best


def raw_height(x, z):
    h = 520.0 * fbm(x, z, 9000.0, 3, 11)           # long rolling swells
    h += 60.0 * fbm(x, z, 1500.0, 3, 37)           # small undulation
    r = math.hypot(x - BARN[0], z - BARN[1])
    h += 380.0 * math.exp(-(r / 3800.0) ** 2)      # the rise the barn stands on
    h += 700.0 * smoothstep(-5500.0, -12000.0, z)  # ground climbs to the tree line behind
    h -= 150.0 * smoothstep(0.0, 9000.0, z)        # foreground dips a little
    return h


PAD_H = raw_height(*BARN)


def height(x, z):
    h = raw_height(x, z)
    r = math.hypot(x - BARN[0], z - BARN[1])
    k = 1.0 - smoothstep(BARN_PAD, BARN_PAD + 900.0, r)
    h = h + (PAD_H - h) * k
    # the path is worn a few cm into the grass
    h -= 6.0 * (1.0 - smoothstep(60.0, 160.0, path_distance(x, z)))
    return h


def path_weight(x, z):
    d = path_distance(x, z)
    # FIELD-BARN-2: a ragged, fairly crisp edge (grass bites into the dirt at metre and decimetre scale)
    e = 100.0 + 80.0 * fbm(x, z, 260.0, 3, 5) + 35.0 * fbm(x, z, 60.0, 2, 7)
    w = 1.0 - smoothstep(e - 22.0, e + 12.0, d)
    r = math.hypot(x - BARN[0], z - (BARN[1] + 900.0))  # trampled apron in front of the door
    ea = 520.0 + 160.0 * fbm(x, z, 250.0, 2, 9) + 30.0 * fbm(x, z, 60.0, 2, 13)
    w = max(w, 1.0 - smoothstep(ea - 40.0, ea + 20.0, r))
    return w


# ── .dlht (container v3) ──
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


def sample_from_cm(hcm):
    return max(0, min(65535, int(round(hcm / ZSCALE * 128.0 + 32768.0))))


def write_tile(path, tx, tz):
    n = QUADS + 1
    hs, wg, wd, wy = [], bytearray(), bytearray(), bytearray()
    for j in range(n):
        z = ORIGIN + (tz * QUADS + j) * SPACING
        for i in range(n):
            x = ORIGIN + (tx * QUADS + i) * SPACING
            hs.append(sample_from_cm(height(x, z)))
            p = int(round(255 * path_weight(x, z)))
            # FIELD-GROUND: dry patches 2-6 m across (5 m noise step, a 30 m field only clusters them), a soft ~1 m
            # edge, ~18 % of the grass area dry (sampled: chord median 2.2 m, p90 6 m)
            dry = smoothstep(0.15, 0.35, fbm(x, z, 500.0, 3, 23) + 0.3 * fbm(x, z, 3000.0, 2, 29))
            dv = int(round((255 - p) * dry))
            wd.append(p); wy.append(dv); wg.append(255 - p - dv)
    planes = [(b"FieldDirt", wd), (b"FieldGrass", wg), (b"FieldDry", wy)]

    def weight_section():
        out = struct.pack("<I", len(planes))
        for name, w in planes:
            out += struct.pack("<I", len(name)) + name + bytes(w)
        return out

    hb = struct.pack("<%dH" % len(hs), *hs)
    body = struct.pack("<4sIIIIQ", b"DLHT", 3, n, n, 1, len(hb)) + hb + weight_section()
    body += struct.pack("<QI", EDIT_LAYER_GUID, len(hs)) + hb + weight_section()
    body += struct.pack("<I", crc32c(body))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(body)
