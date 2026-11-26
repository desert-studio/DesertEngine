"""Gives every opacity texture of a masked material the alpha coverage its material clips at.

The importer sets a texture's AlphaCoverageThresholds from the material that clips it (ImportManager: a material with
OpacityMaskClipValue > 0 marks its OpacityTexture's OpacityChannel with the clip value), and the cook then keeps that
coverage in every mip (TextureBinary PreserveAlphaCoverage, UE bPreserveAlphaCoverage). Textures imported before that
rule carry zero thresholds, so their low mips lose coverage and a masked leaf card thins out as the mip shifts
(Textures.MipBias, streaming). This applies the importer's rule to the texture assets already in a project, in place:
only the IMPT section's four thresholds change (its section hash and the header CRC are rewritten with them).

usage: python scripts/Content/alpha_coverage_from_materials.py <project Content dir> [--check]
  --check: write nothing; exit 1 when a texture would change.
"""
import json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from content_files import crc32c, fnv1a64  # noqa: E402

TOC_ENTRY = 32


def param(material, name):
    for p in material.get("Params", []):
        if p.get("Name") == name:
            return p.get("Value")
    return None


def texture(material, name):
    for t in material.get("Textures", []):
        if t.get("Name") == name and t.get("Path"):
            return t["Path"]
    return None


def wanted_thresholds(content_dir):
    """texture file -> (thresholds[4], [materials that ask])."""
    wanted = {}
    for root, _, files in os.walk(content_dir):
        for name in files:
            if not name.endswith(".demat"):
                continue
            path = os.path.join(root, name)
            with open(path, encoding="utf-8") as f:
                material = json.load(f)
            clip = param(material, "OpacityMaskClipValue")
            opacity = texture(material, "OpacityTexture")
            if clip is None or not clip[0] > 0.0 or opacity is None:
                continue
            if not opacity.startswith("assets:"):
                raise SystemExit("%s: OpacityTexture '%s' is not an assets: path" % (path, opacity))
            channel = param(material, "OpacityChannel")
            at = min(max(int(channel[0]) if channel is not None else 3, 0), 3)
            file = os.path.normpath(os.path.join(content_dir, opacity[len("assets:"):]))
            thresholds, askers = wanted.setdefault(file, ([0.0] * 4, []))
            # As the engine holds it: a float (the text may spell the same float as 0.35 or 0.3499999940395355).
            value = struct.unpack("<f", struct.pack("<f", min(max(float(clip[0]), 0.0), 1.0)))[0]
            if thresholds[at] not in (0.0, value):
                raise SystemExit("%s channel %d: clipped at %g by %s and at %g by %s" %
                                 (file, at, thresholds[at], askers, value, path))
            thresholds[at] = value
            askers.append(path)
    return wanted


def read_string(data, at):
    n, = struct.unpack_from("<I", data, at)
    return at + 4 + n


def thresholds_offset(impt):
    """The byte offset of the four thresholds inside an IMPT section (TextureSourceAsset EncodeTextureImportInfo)."""
    version, = struct.unpack_from("<I", impt, 0)
    if version < 3:
        raise ValueError("IMPT version %d has no alpha coverage thresholds" % version)
    at = read_string(impt, 4)  # SourceFile
    at += 8                    # SourceHash
    at = read_string(impt, at)  # Intent
    at = read_string(impt, at)  # ColorSpace
    return at


def sections(data):
    header_size, = struct.unpack_from("<I", data, 8)
    at = 32
    kind_len, = struct.unpack_from("<H", data, at)
    at += 2 + kind_len
    count, = struct.unpack_from("<I", data, at)
    at += 4 + 8 * count
    count, = struct.unpack_from("<I", data, at)
    at += 4 + 16 * count
    count, = struct.unpack_from("<I", data, at)
    at += 4
    rows = []
    for i in range(count):
        row = at + TOC_ENTRY * i
        tag = data[row:row + 4]
        codec, off, size, digest = struct.unpack_from("<IQQQ", data, row + 4)
        rows.append((tag, row, codec, off, size, digest))
    if at + TOC_ENTRY * count != header_size:
        raise ValueError("the TOC ends at %d, the header says %d" % (at + TOC_ENTRY * count, header_size))
    return header_size, rows


def check_envelope(data, path):
    header_size, rows = sections(data)
    header = bytearray(data[:header_size])
    stored, = struct.unpack_from("<I", header, 12)
    header[12:16] = b"\0\0\0\0"
    if crc32c(header) != stored:
        raise ValueError("%s: header CRC mismatch" % path)
    for tag, _, _, off, size, digest in rows:
        if fnv1a64(data[off:off + size]) != digest:
            raise ValueError("%s: section %s hash mismatch" % (path, tag))


def apply(path, thresholds):
    """Returns the old thresholds when they change, None when already set."""
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if data[:4] != b"DAST":
        raise ValueError("%s is not an asset envelope" % path)
    check_envelope(data, path)
    header_size, rows = sections(data)
    impt = [r for r in rows if r[0] == b"IMPT"]
    if not impt:
        raise ValueError("%s has no IMPT section" % path)
    _, row, codec, off, size, _ = impt[0]
    if codec != 0:
        raise ValueError("%s: IMPT is stored with codec %d, not raw" % (path, codec))
    at = off + thresholds_offset(bytes(data[off:off + size]))
    old = list(struct.unpack_from("<4f", data, at))
    new = [struct.unpack("<f", struct.pack("<f", t))[0] for t in thresholds]
    if old == new:
        return None
    struct.pack_into("<4f", data, at, *thresholds)
    struct.pack_into("<Q", data, row + 24, fnv1a64(data[off:off + size]))
    data[12:16] = b"\0\0\0\0"
    struct.pack_into("<I", data, 12, crc32c(data[:header_size]))
    check_envelope(data, path)
    return old, bytes(data)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    content_dir = os.path.abspath(argv[1])
    check_only = "--check" in argv[2:]
    changed = 0
    for file, (thresholds, askers) in sorted(wanted_thresholds(content_dir).items()):
        if not os.path.isfile(file):
            raise SystemExit("%s (OpacityTexture of %s) does not exist" % (file, askers[0]))
        result = apply(file, thresholds)
        if result is None:
            continue
        old, data = result
        changed += 1
        print("%s: %s -> %s (%d material(s))" % (os.path.relpath(file, content_dir), old, thresholds, len(askers)))
        if not check_only:
            with open(file, "wb") as f:
                f.write(data)
    print("%d texture(s) %s" % (changed, "would change" if check_only else "changed"))
    return 1 if check_only and changed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
