#!/usr/bin/env python3
"""Per-triangle tangent-frame check of a cooked mesh (.skmesh / .stmesh, DESTMESH v5).

For every triangle with a non-degenerate UV mapping it derives the face frame from the triangle itself,
T = dPos/dU and B = dPos/dV (the frame MikkTSpace builds before averaging), and compares each corner's
STORED Tangent and Bitangent against it after projecting both onto the corner's tangent plane. A triangle
counts as a mismatch when any corner's stored vector points more than --max-angle degrees away from the
face's. Zero-area triangles have no frame: they are counted and skipped. Prints the counts; exit code 1 when any triangle mismatches.

    python scripts/Dev/mesh_tangent_check.py Editor/Resources/Engine/Meshes/Skinned/Humanoid.skmesh
"""
import argparse
import math
import struct
import sys

MAGIC = b"DESTMESH"
PREFIX = 96  # 80-byte header + 16-byte asset GUID (MeshBinaryHeader.hpp)
ROW = struct.Struct("<IIQQ")  # {Id, ElementSize, Offset, Count}
SEC_STATIC, SEC_SKINNED, SEC_INDICES = 1, 2, 3
VERTEX_FLOATS = 14  # Position, Normal, Tangent, Bitangent (vec3 each), TexCoord (vec2)


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def scale(a, s): return (a[0] * s, a[1] * s, a[2] * s)


def on_plane(v, n):
    p = sub(v, scale(n, dot(v, n)))
    length = math.sqrt(dot(p, p))
    return scale(p, 1.0 / length) if length > 1e-12 else None


def read_mesh(path):
    data = open(path, "rb").read()
    if data[:8] != MAGIC:
        sys.exit(f"{path}: not a DESTMESH container")
    version, = struct.unpack_from("<I", data, 12)
    if version != 5:
        sys.exit(f"{path}: container version {version}, this check reads 5")
    count, = struct.unpack_from("<I", data, 24)
    sections = {}
    for i in range(count):
        sid, size, offset, n = ROW.unpack_from(data, PREFIX + i * ROW.size)
        sections[sid] = (size, offset, n)
    size, offset, n = sections[SEC_SKINNED] if sections.get(SEC_SKINNED, (0, 0, 0))[2] else sections[SEC_STATIC]
    verts = [struct.unpack_from(f"<{VERTEX_FLOATS}f", data, offset + i * size) for i in range(n)]
    isize, ioffset, icount = sections[SEC_INDICES]
    tris = [struct.unpack_from("<3I", data, ioffset + i * isize) for i in range(icount)]
    return verts, tris


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mesh")
    ap.add_argument("--max-angle", type=float, default=45.0, help="degrees (default 45)")
    args = ap.parse_args()
    cos_limit = math.cos(math.radians(args.max_angle))

    verts, tris = read_mesh(args.mesh)
    checked = pos_degenerate = uv_degenerate = bad_t = bad_b = bad_any = 0
    for tri in tris:
        v = [verts[i] for i in tri]
        p = [x[0:3] for x in v]
        uv = [x[12:14] for x in v]
        e1, e2 = sub(p[1], p[0]), sub(p[2], p[0])
        du1, dv1 = uv[1][0] - uv[0][0], uv[1][1] - uv[0][1]
        du2, dv2 = uv[2][0] - uv[0][0], uv[2][1] - uv[0][1]
        cross = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        if dot(cross, cross) <= (1e-6 * max(dot(e1, e1), dot(e2, e2), 1e-30)) ** 2:
            pos_degenerate += 1  # zero-area triangle: no surface, no frame (MikkTSpace skips it too)
            continue
        det = du1 * dv2 - du2 * dv1
        if abs(det) < 1e-12:
            uv_degenerate += 1
            continue
        r = 1.0 / det
        face_t = scale(sub(scale(e1, dv2), scale(e2, dv1)), r)
        face_b = scale(sub(scale(e2, du1), scale(e1, du2)), r)
        t_wrong = b_wrong = False
        for x in v:
            n = x[3:6]
            ft, fb = on_plane(face_t, n), on_plane(face_b, n)
            st, sb = on_plane(x[6:9], n), on_plane(x[9:12], n)
            if ft is None or fb is None:
                continue
            t_wrong |= st is None or dot(st, ft) < cos_limit
            b_wrong |= sb is None or dot(sb, fb) < cos_limit
        checked += 1
        bad_t += t_wrong
        bad_b += b_wrong
        bad_any += t_wrong or b_wrong

    pct = 100.0 * bad_any / checked if checked else 0.0
    print(f"{args.mesh}: {len(verts)} vertices, {len(tris)} triangles, {checked} checked, "
          f"{pos_degenerate} zero-area and {uv_degenerate} degenerate-UV skipped")
    print(f"  tangent mismatches {bad_t}, bitangent mismatches {bad_b}, "
          f"triangles with either {bad_any} ({pct:.1f}%) at > {args.max_angle:g} deg")
    return 1 if bad_any else 0


if __name__ == "__main__":
    sys.exit(main())
