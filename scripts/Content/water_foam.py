#!/usr/bin/env python3
# Usage: python3 scripts/Content/water_foam.py Projects/Desert/Content/Water/Textures  (then re-import the PNGs)
# Authors the foam's structure maps (own work, CC0): T_WaterFoam_H = the height of a foam raft (R the large cells'
# walls, G the small bubbles, B the thickness), T_WaterFoam_N = the normal of R + G, OpenGL (+Y up). Tileable.
import sys
import numpy as np
sys.dont_write_bytecode = True
import struct, zlib

N = 1024


def png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def cells(grid, seed):
    """Distance to the wall between the two nearest seeds of a jittered, wrapping grid, in cells."""
    rnd = np.random.default_rng(seed)
    px, py = rnd.random((grid, grid)), rnd.random((grid, grid))
    t = (np.arange(N) + 0.5) / N * grid
    x, y = np.meshgrid(t, t)
    cx, cy = np.floor(x).astype(int), np.floor(y).astype(int)
    f1 = np.full((N, N), 9.0)
    f2 = np.full((N, N), 9.0)
    for oy in range(-2, 3):
        for ox in range(-2, 3):
            gx, gy = cx + ox, cy + oy
            dx = gx + px[gy % grid, gx % grid] - x
            dy = gy + py[gy % grid, gx % grid] - y
            d = np.sqrt(dx * dx + dy * dy)
            f2 = np.minimum(f2, np.maximum(d, f1))
            f1 = np.minimum(f1, d)
    return f2 - f1


def smooth_noise(grid, seed):
    rnd = np.random.default_rng(seed)
    spectrum = np.zeros((N, N), complex)
    k = np.fft.fftfreq(N) * N
    kx, ky = np.meshgrid(k, k)
    spectrum = np.exp(-(kx * kx + ky * ky) / (2.0 * grid * grid)) * np.exp(2j * np.pi * rnd.random((N, N)))
    field = np.real(np.fft.ifft2(spectrum))
    return (field - field.mean()) / field.std()


def to_mean(h, mean):
    """The power that brings the mean of a 0..1 field to `mean`."""
    lo, hi = 0.05, 20.0
    for _ in range(60):
        mid = np.sqrt(lo * hi)
        if (h ** mid).mean() > mean:
            lo = mid
        else:
            hi = mid
    return h ** np.sqrt(lo * hi)


out = sys.argv[1]
warp = smooth_noise(6, 3)
large = np.exp(-np.maximum(cells(11, 11) + 0.04 * warp, 0.0) / 0.22)
small = np.exp(-cells(53, 23) / 0.28)
body = np.clip(0.5 + 0.28 * smooth_noise(5, 7) + 0.08 * smooth_noise(24, 9), 0.0, 1.0)
r = to_mean(np.clip(0.72 * large + 0.28 * small * body + 0.12 * (body - 0.5), 0.0, 1.0), 0.45)
g = to_mean(np.clip(small, 0.0, 1.0), 0.45)
b = np.clip(0.3 + 0.7 * r * body, 0.0, 1.0)
png(out + '/T_WaterFoam_H.png', np.round(np.stack([r, g, b], -1) * 255).astype(np.uint8))
height = (0.7 * r + 0.3 * g) * 6.0  # texels
dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5
dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5
n = np.stack([-dx, dy, np.ones_like(dx)], -1)  # image v runs down: +Y (GL) is up the image
n /= np.linalg.norm(n, axis=-1, keepdims=True)
png(out + '/T_WaterFoam_N.png', np.round((n * 0.5 + 0.5) * 255).astype(np.uint8))
print('mean', r.mean(), g.mean(), 'slope rms', float(np.sqrt((dx * dx + dy * dy).mean())))
