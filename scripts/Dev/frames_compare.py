#!/usr/bin/env python3
"""frames_compare.py <a.png> <b.png> <diff.png> — pixel comparison of two captures that are NOT byte-identical.

Prints one line: differing pixel share, max |delta| (0-255, any channel), mean |delta| over differing pixels, the
bounding box of the difference, and writes <diff.png>: the frame darkened to 25 % with every differing pixel
painted red at a brightness proportional to its delta (x8, so a delta of 1 is still visible).
Exit 0 = pixels equal (the PNG bytes differed only in encoding), 1 = pixels differ, 2 = cannot compare.
Needs numpy and Pillow; a directory holding them may be named in DESERT_PYLIB (on the Windows bench machine:
F:/rdg-baseline/_tools/pylib).
"""
import os
import sys

if os.environ.get("DESERT_PYLIB"):
    sys.path.insert(0, os.environ["DESERT_PYLIB"])
try:
    import numpy as np
    from PIL import Image
except ImportError as e:
    print(f"frames_compare: {e}; install numpy+Pillow or point DESERT_PYLIB at a directory holding them")
    sys.exit(2)


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[0])
        return 2
    a_path, b_path, diff_path = sys.argv[1:]
    a = np.asarray(Image.open(a_path).convert("RGBA")).astype(np.int16)
    b = np.asarray(Image.open(b_path).convert("RGBA")).astype(np.int16)
    if a.shape != b.shape:
        print(f"size differs: {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}")
        return 1
    delta = np.abs(a - b).max(axis=2)
    mask = delta > 0
    n = int(mask.sum())
    if n == 0:
        print("pixels equal (PNG encoding differs)")
        return 0
    ys, xs = np.nonzero(mask)
    out = (a[:, :, :3] // 4).astype(np.uint8)
    out[mask] = np.stack([np.minimum(255, 64 + delta[mask] * 8), np.zeros(n), np.zeros(n)], axis=1).astype(np.uint8)
    Image.fromarray(out, "RGB").save(diff_path)
    print(f"{n} px ({100.0 * n / mask.size:.3f}%) differ, max delta {int(delta.max())}, mean {delta[mask].mean():.2f}, "
          f"bbox {xs.min()},{ys.min()}-{xs.max()},{ys.max()}")
    return 1


sys.exit(main())
