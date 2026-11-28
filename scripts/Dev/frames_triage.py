#!/usr/bin/env python3
"""frames_triage.py RUN_DIR [REF_DIR] [--floor F] — which frames of a scenario run must be opened as images.

One line per PNG of RUN_DIR, numbers only, no image is shown:
  black      the frame's mean brightness is under 2/255 (a failed capture: FAIL without opening it);
  pair       NAME_a / NAME_b of the same run: mean and max |delta| (0-255) between the two;
  ref        the same file name in REF_DIR (an accepted run): mean and max |delta| against it.
Verdict per frame: «same» = its reference (else its pair) is at or under the floor (mean |delta| <= F, default 0.5),
«LOOK» = it differs, has no comparison, or is black. Only «LOOK» frames are opened; a «same» frame inherits the
reference's verdict (without REF_DIR, the _b of an equal pair inherits its _a's). Exit 0 always when it could read the frames, 2 when it cannot compare.
Needs numpy and Pillow (DESERT_PYLIB may name a directory holding them, as for frames_compare.py).
"""
import os
import sys

if os.environ.get("DESERT_PYLIB"):
    sys.path.insert(0, os.environ["DESERT_PYLIB"])
try:
    import numpy as np
    from PIL import Image
except ImportError as e:
    print(f"frames_triage: {e}; install numpy+Pillow or point DESERT_PYLIB at a directory holding them")
    sys.exit(2)


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int16)


def delta(a, b):
    if a.shape != b.shape:
        return None
    d = np.abs(a - b)
    return float(d.mean()), int(d.max())


def main(argv):
    floor = 0.5
    if "--floor" in argv:
        i = argv.index("--floor")
        floor = float(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]
    if not argv or len(argv) > 2 or not os.path.isdir(argv[0]):
        print(__doc__.strip().splitlines()[0])
        return 2
    run, ref = argv[0], (argv[1] if len(argv) == 2 else None)
    names = sorted(n for n in os.listdir(run) if n.lower().endswith(".png"))
    if not names:
        print(f"frames_triage: no PNG in {run}")
        return 2
    frames = {n: load(os.path.join(run, n)) for n in names}
    look = 0
    for n in names:
        img, parts, differs = frames[n], [], False
        black = float(img.mean()) < 2.0
        stem = n[:-4]
        if stem.endswith(("_a", "_b")):
            other = stem[:-1] + ("b" if stem.endswith("_a") else "a") + ".png"
            if other in frames:
                d = delta(img, frames[other])
                parts.append("pair size differs" if d is None else f"pair {d[0]:.2f}/{d[1]}")
                differs |= d is None or d[0] > floor
        if ref and os.path.exists(os.path.join(ref, n)):  # the accepted frame decides: its pair was judged with it
            d = delta(img, load(os.path.join(ref, n)))
            parts.append("ref size differs" if d is None else f"ref {d[0]:.2f}/{d[1]}")
            differs = d is None or d[0] > floor
        judged = any(x.startswith("ref") for x in parts) or (stem.endswith("_b") and parts)  # _a is opened for both
        verdict = "LOOK" if black or differs or not judged else "same"
        look += verdict == "LOOK"
        print(f"{verdict:4}  {n}  " + ("black  " if black else "") + ("  ".join(parts) or "no comparison"))
    print(f"{look} of {len(names)} frame(s) to open (floor mean |delta| {floor})")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
