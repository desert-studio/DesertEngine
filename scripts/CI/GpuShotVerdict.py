#!/usr/bin/env python3
"""Verdict on one headless GPU frame: the picture is not black and the run logged no Vulkan validation error.

Usage: GpuShotVerdict.py <shot.png> <run.log> [<run.log> ...]

Exit 0 when the PNG decodes, its mean channel value (RGB, alpha ignored) is above zero, and no log line is a
validation error; exit 1 otherwise, naming every reason. The mean is printed either way so the CI log and a
local run report the same number.

A validation error is any line carrying a VUID (the validation layer names every rule it checks by one), a
synchronization-validation hazard id (SYNC-HAZARD-*, enabled by VulkanContext for the render graph), the
layer's own "Validation Error" prefix, or vk-bootstrap's default messenger header "[ERROR: Validation]".
Warnings and performance notes are not counted: the gate is on rule violations, not advice.

Standard library only (zlib + struct), so the self-hosted runner needs nothing but Python 3.
"""

import re
import struct
import sys
import zlib

VALIDATION_ERROR = re.compile(r"VUID-|SYNC-HAZARD-|Validation Error|\[ERROR: Validation\]")


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def png_mean(path):
    """Mean of the colour channels of an 8-bit, non-interlaced, greyscale/RGB/RGBA PNG."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    pos, idat, header = 8, bytearray(), None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos : pos + 8])
        body = data[pos + 8 : pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length
    if header is None:
        raise ValueError(f"{path} has no IHDR")
    width, height, depth, colour, _, _, interlace = header
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour)
    if depth != 8 or channels is None or interlace != 0:
        raise ValueError(f"{path}: unsupported PNG (depth {depth}, colour type {colour}, interlace {interlace})")
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    colour_channels = 3 if channels >= 3 else 1
    prev = bytearray(stride)
    total = 0
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        row = bytearray(raw[start + 1 : start + 1 + stride])
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = prev[i]
            upleft = prev[i - channels] if i >= channels else 0
            if kind == 1:
                row[i] = (row[i] + left) & 0xFF
            elif kind == 2:
                row[i] = (row[i] + up) & 0xFF
            elif kind == 3:
                row[i] = (row[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                row[i] = (row[i] + paeth(left, up, upleft)) & 0xFF
            elif kind != 0:
                raise ValueError(f"{path}: bad filter {kind} on row {y}")
        for c in range(colour_channels):
            total += sum(row[c::channels])
        prev = row
    return width, height, total / float(width * height * colour_channels)


def validation_lines(paths):
    hits = []
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for number, line in enumerate(f, 1):
                if VALIDATION_ERROR.search(line):
                    hits.append(f"{path}:{number}: {line.rstrip()}")
    return hits


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[2])
        return 2
    shot, logs = argv[1], argv[2:]
    failures = []
    try:
        width, height, mean = png_mean(shot)
        print(f"frame {shot}: {width}x{height}, mean {mean:.3f}")
        if mean <= 0.0:
            failures.append(f"the frame is black (mean {mean})")
    except (OSError, ValueError, zlib.error) as error:
        failures.append(f"frame unreadable: {error}")
    hits = validation_lines(logs)
    print(f"validation errors: {len(hits)}")
    for hit in hits[:50]:
        print(hit)
    if hits:
        failures.append(f"{len(hits)} Vulkan validation error line(s)")
    for failure in failures:
        print(f"FAIL: {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
