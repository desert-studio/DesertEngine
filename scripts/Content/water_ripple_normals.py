#!/usr/bin/env python3
# Usage: python3 scripts/Content/water_ripple_normals.py Projects/Desert/Content/Water/Textures  (then re-import the PNGs)
# Authors the two CC0 ripple normal maps of the water material (no CC0 library ships a water normal map):
# a tileable height field from a Phillips wind-wave spectrum on the integer wave-vector lattice (Tessendorf 2001),
# random phases, its analytic gradient packed as an OpenGL tangent-space normal (+Y up). Pure Python (no numpy/PIL).
import math, random, struct, sys, zlib

def png(path, w, h, rows):
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

def author(path, n, seed, kmin, kmax, wind_deg, spread, slope):
    rnd = random.Random(seed)
    wd = (math.cos(math.radians(wind_deg)), math.sin(math.radians(wind_deg)))
    peak = kmin * 2.0
    waves = []
    for kx in range(-kmax, kmax + 1):
        for ky in range(-kmax, kmax + 1):
            k = math.hypot(kx, ky)
            if k < kmin or k > kmax:
                continue
            c = (kx * wd[0] + ky * wd[1]) / k
            # Phillips: exp(-1/(kL)^2)/k^4 * |k.w|^spread, small-wave cut; amplitude of the SLOPE (k*a) matters.
            p = math.exp(-(peak / k) ** 2) / k ** 4 * (abs(c) ** spread) * math.exp(-(k / kmax) ** 2)
            if p < 1e-12:
                continue
            a = math.sqrt(p) * rnd.gauss(1.0, 0.35)
            waves.append((kx, ky, a, rnd.uniform(0, 2 * math.pi)))
    waves.sort(key=lambda w: -abs(w[2]) * math.hypot(w[0], w[1]))
    waves = waves[:600]
    sin_t = [math.sin(2 * math.pi * i / n) for i in range(n)]
    gx = [[0.0] * n for _ in range(n)]
    gy = [[0.0] * n for _ in range(n)]
    for kx, ky, a, ph in waves:
        off = int(ph / (2 * math.pi) * n)
        sx, sy = a * 2 * math.pi * kx, a * 2 * math.pi * ky
        for y in range(n):
            base = (ky * y + off) % n
            rx, ry = gx[y], gy[y]
            for x in range(n):
                s = sin_t[(kx * x + base) % n]
                rx[x] -= sx * s
                ry[x] -= sy * s
    rms = math.sqrt(sum(v * v for r in gx for v in r) / (n * n) + sum(v * v for r in gy for v in r) / (n * n))
    g = slope / rms
    rows = []
    for y in range(n):
        row = []
        for x in range(n):
            dx, dy = gx[y][x] * g, gy[y][x] * g
            l = math.sqrt(dx * dx + dy * dy + 1.0)
            nx, ny, nz = -dx / l, dy / l, 1.0 / l  # image v runs down: +Y (GL) is up the image
            row += [int(round((nx * 0.5 + 0.5) * 255)), int(round((ny * 0.5 + 0.5) * 255)), int(round((nz * 0.5 + 0.5) * 255))]
        rows.append(row)
    png(path, n, n, rows)
    print(path, len(waves), 'waves')

out = sys.argv[1]
author(out + "/T_WaterRipple_A_N.png", 512, 11, 2, 32, 0.0, 2, 0.16)
author(out + '/T_WaterRipple_B_N.png', 512, 29, 3, 40, 35.0, 2, 0.12)
