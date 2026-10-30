#!/usr/bin/env python3
# Usage: python3 scripts/Content/foliage_canopy_normals.py <tree.gltf> [--material-suffix _leaves] [--weight 1.0]
#        (then re-import: the .gltf's bytes key the imported mesh source, so the edited JSON is a DDC miss)
# Bakes a CANOPY NORMAL into a tree's leaf cards, in the asset (UE / SpeedTree foliage practice: leaf-card normals
# transferred from a proxy volume around the crown, so the crown shades and takes snow as one volume rather than as
# a thousand cards facing every way). Per card vertex the normal becomes the outward normal of the crown's
# ellipsoid (the leaf vertices' bounding box), blended with the card's own normal by --weight (1 = the ellipsoid
# only). Every leaf triangle is also wound so its FRONT faces outward along that normal: a two-sided material
# negates the normal on the back face (SurfaceTypes.glslh DesertSurfaceInput, UE's TwoSidedSign), so a card wound
# inward would show the crown's outside as its inside - the canopy normal pointing down on the crown's top.
# Per leaf vertex it also bakes the CANOPY DEPTH: how far (cm) the vertex hangs below its crown's top surface (the
# highest leaf over its (x, z) neighbourhood, in the frame the plant stands in), as COLOR_0.a = 1 - depth / 1024 cm.
# The surface's snow / rain occlusion is looked up that far up (SurfaceWeather.glslh DesertCanopyLift), so a card
# reads what OTHER casters hold over its crown rather than its own crown's top (UE foliage snow: a world-up mask on
# the crown volume, no self-occlusion inside it). A file may hold several plants (one crown per node).
# The source .bin is not touched: the new normals, colours and indices go to a sidecar buffer <name>_canopy.bin, and the
# leaf primitives' NORMAL / indices accessors are repointed to it; the original accessor ids are kept in the
# primitive's extras, so a re-run bakes from the original data again. Pure Python (no numpy).
import argparse, json, math, os, struct, sys
from array import array

COMPONENT = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
# Must equal kDesertCanopyDepthRangeCm (Editor/Resources/Shaders/Mesh/Surface/SurfaceWeather.glslh).
CANOPY_DEPTH_RANGE_CM = 1024.0


def read_accessor(gltf, buffers, index):
    acc = gltf['accessors'][index]
    view = gltf['bufferViews'][acc['bufferView']]
    code = COMPONENT[acc['componentType']]
    n = acc['count'] * WIDTH[acc['type']]
    stride = view.get('byteStride', 0)
    if stride and stride != struct.calcsize(code) * WIDTH[acc['type']]:
        sys.exit(f"accessor {index}: interleaved buffer views are not supported")
    start = view.get('byteOffset', 0) + acc.get('byteOffset', 0)
    data = array(code)
    data.frombytes(buffers[view['buffer']][start:start + n * data.itemsize])
    if sys.byteorder != 'little':
        data.byteswap()
    return data


def node_worlds(gltf):
    """Each node's 3x4 world matrix (rows), composed down from the scene roots."""
    nodes = gltf.get('nodes', [])

    def local(n):
        if 'matrix' in n:
            mm = n['matrix']  # column-major
            return [[mm[c * 4 + r] for c in range(4)] for r in range(3)]
        x, y, z, qw = n.get('rotation', [0, 0, 0, 1])
        sx, sy, sz = n.get('scale', [1, 1, 1])
        t = n.get('translation', [0, 0, 0])
        rot = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * qw), 2 * (x * z + y * qw)],
               [2 * (x * y + z * qw), 1 - 2 * (x * x + z * z), 2 * (y * z - x * qw)],
               [2 * (x * z - y * qw), 2 * (y * z + x * qw), 1 - 2 * (x * x + y * y)]]
        return [[rot[r][0] * sx, rot[r][1] * sy, rot[r][2] * sz, t[r]] for r in range(3)]

    def mul(a, b):
        return [[sum(a[r][k] * b[k][c] for k in range(3)) + (a[r][3] if c == 3 else 0.0) for c in range(4)]
                for r in range(3)]

    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', []):
            parent[c] = i
    worlds = {}

    def world(i):
        if i not in worlds:
            worlds[i] = mul(world(parent[i]), local(nodes[i])) if i in parent else local(nodes[i])
        return worlds[i]

    return [world(i) for i in range(len(nodes))]


def determinant3(m):
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
            m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


def invert3(m):
    d = determinant3(m)
    if abs(d) < 1e-12:
        sys.exit("a crown node's transform is singular")
    return [[(m[(c + 1) % 3][(r + 1) % 3] * m[(c + 2) % 3][(r + 2) % 3] -
              m[(c + 1) % 3][(r + 2) % 3] * m[(c + 2) % 3][(r + 1) % 3]) / d for c in range(3)] for r in range(3)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gltf')
    ap.add_argument('--material-suffix', default='_leaves')
    ap.add_argument('--weight', type=float, default=1.0)
    ap.add_argument('--cell-cm', type=float, default=25.0)
    ap.add_argument('--unit-cm', type=float, default=100.0)  # glTF positions are metres
    args = ap.parse_args()

    base = os.path.dirname(os.path.abspath(args.gltf))
    stem = os.path.splitext(os.path.basename(args.gltf))[0]
    with open(args.gltf) as f:
        gltf = json.load(f)
    sidecar = f'{stem}_canopy.bin'
    # A re-run replaces the sidecar buffer it made, never appends a second one.
    buffers_json = [b for b in gltf['buffers'] if b.get('uri') != sidecar]
    if len(buffers_json) != len(gltf['buffers']):
        for view in gltf['bufferViews']:
            if gltf['buffers'][view['buffer']].get('uri') == sidecar:
                view['buffer'] = -1
    buffers = []
    for b in buffers_json:
        with open(os.path.join(base, b['uri']), 'rb') as f:
            buffers.append(f.read())

    # One CROWN per node: a file may hold several plants (shrub_02: four bushes, each its own node, each rotated),
    # and the importer bakes each node's transform into its mesh, so a crown is measured in the node's world frame
    # (the frame the plant stands in) and its normals are written back in the mesh's own frame.
    world = node_worlds(gltf)
    crowns = {}
    for node_id, node in enumerate(gltf.get('nodes', [])):
        if 'mesh' not in node:
            continue
        for prim in gltf['meshes'][node['mesh']]['primitives']:
            name = gltf['materials'][prim['material']]['name'] if 'material' in prim else ''
            if not name.endswith(args.material_suffix):
                continue
            if prim.get('mode', 4) != 4:
                sys.exit(f"{name}: only triangle lists are supported")
            if '_canopy_node' in prim and prim['_canopy_node'] != node_id:
                sys.exit(f"{name}: mesh {node['mesh']} is instanced by two nodes; a crown needs its own mesh")
            prim['_canopy_node'] = node_id
            extras = prim.get('extras', {})
            original = extras.get('CanopyOriginal', {})
            if 'COLOR_0' in prim['attributes'] and 'COLOR_0' not in extras.get('CanopyBaked', []):
                sys.exit(f"{name}: has authored vertex colours; the canopy depth would overwrite them")
            crowns.setdefault(node_id, []).append(
                (prim, original.get('NORMAL', prim['attributes']['NORMAL']), original.get('indices', prim['indices'])))
    if not crowns:
        sys.exit(f"no node draws a primitive whose material ends with '{args.material_suffix}'")

    w = min(max(args.weight, 0.0), 1.0)
    out = bytearray()
    new_views, new_accessors = [], []
    report = []
    for node_id, leaves in crowns.items():
        m = world[node_id]
        lin = [[m[r][c] for c in range(3)] for r in range(3)]
        inv_lin = invert3(lin)
        local = [read_accessor(gltf, buffers, p['attributes']['POSITION']) for p, _, _ in leaves]
        positions = []
        for pos in local:
            wp = array('f', bytes(len(pos) * 4))
            for o in range(0, len(pos), 3):
                for r in range(3):
                    wp[o + r] = m[r][0] * pos[o] + m[r][1] * pos[o + 1] + m[r][2] * pos[o + 2] + m[r][3]
            positions.append(wp)
        lo = [min(min(pos[k::3]) for pos in positions) for k in range(3)]
        hi = [max(max(pos[k::3]) for pos in positions) for k in range(3)]
        centre = [(lo[k] + hi[k]) * 0.5 for k in range(3)]
        inv_r2 = [1.0 / max((hi[k] - lo[k]) * 0.5, 1e-6) ** 2 for k in range(3)]

        # The crown's TOP SURFACE as a height field over (x, z): the highest leaf vertex per cell, dilated by a
        # cell so a card reads the top of its neighbourhood (the occlusion map's footprint is wider than a card).
        cell = args.cell_cm / args.unit_cm
        top = {}
        for pos in positions:
            for o in range(0, len(pos), 3):
                key = (math.floor(pos[o] / cell), math.floor(pos[o + 2] / cell))
                if pos[o + 1] > top.get(key, -math.inf):
                    top[key] = pos[o + 1]
        crown_top = {}
        for (cx, cz) in top:
            crown_top[(cx, cz)] = max(top.get((cx + dx, cz + dz), -math.inf) for dx in (-1, 0, 1) for dz in (-1, 0, 1))

        deepest = 0.0
        for (prim, normal_id, index_id), pos in zip(leaves, positions):
            own_local = read_accessor(gltf, buffers, normal_id)
            idx = read_accessor(gltf, buffers, index_id)
            count = len(pos) // 3
            nrm = array('f', bytes(count * 12))
            col = array('f', bytes(count * 16))
            for v in range(count):
                o = 3 * v
                # The crown ellipsoid's outward normal at the vertex (the gradient of its implicit function).
                cx = (pos[o] - centre[0]) * inv_r2[0]
                cy = (pos[o + 1] - centre[1]) * inv_r2[1]
                cz = (pos[o + 2] - centre[2]) * inv_r2[2]
                cl = math.sqrt(cx * cx + cy * cy + cz * cz) or 1.0
                cx, cy, cz = cx / cl, cy / cl, cz / cl
                # The card's own normal in the crown's frame (normal matrix = inverse transpose of the linear
                # part), turned to the canopy's side (its sign is the card's arbitrary winding).
                lx, ly, lz = own_local[o], own_local[o + 1], own_local[o + 2]
                ox, oy, oz = (inv_lin[0][r] * lx + inv_lin[1][r] * ly + inv_lin[2][r] * lz for r in range(3))
                ol = math.sqrt(ox * ox + oy * oy + oz * oz) or 1.0
                ox, oy, oz = ox / ol, oy / ol, oz / ol
                if ox * cx + oy * cy + oz * cz < 0.0:
                    ox, oy, oz = -ox, -oy, -oz
                nx, ny, nz = ox + (cx - ox) * w, oy + (cy - oy) * w, oz + (cz - oz) * w
                nl = math.sqrt(nx * nx + ny * ny + nz * nz)
                if nl < 1e-6:
                    nx, ny, nz, nl = cx, cy, cz, 1.0
                nx, ny, nz = nx / nl, ny / nl, nz / nl
                # Back to the mesh's frame: the inverse of the normal matrix (lin^-1)^T is lin^T.
                bx, by, bz = (lin[0][r] * nx + lin[1][r] * ny + lin[2][r] * nz for r in range(3))
                bl = math.sqrt(bx * bx + by * by + bz * bz) or 1.0
                nrm[o], nrm[o + 1], nrm[o + 2] = bx / bl, by / bl, bz / bl
                # Depth below the crown's top surface (cm), COLOR_0.a = 1 - depth / range (white = at the top).
                key = (math.floor(pos[o] / cell), math.floor(pos[o + 2] / cell))
                depth = max(crown_top[key] - pos[o + 1], 0.0) * args.unit_cm
                deepest = max(deepest, depth)
                c = 4 * v
                col[c], col[c + 1], col[c + 2] = 1.0, 1.0, 1.0
                col[c + 3] = 1.0 - min(depth / CANOPY_DEPTH_RANGE_CM, 1.0)
            # Winding is tested in the crown's frame against the crown-frame normal (a proper rotation keeps the
            # handedness; a mirroring node would flip it, and is refused below).
            tri = array('I', idx)
            flipped = 0
            for t in range(0, len(tri), 3):
                a, b, c = 3 * tri[t], 3 * tri[t + 1], 3 * tri[t + 2]
                e1 = (pos[b] - pos[a], pos[b + 1] - pos[a + 1], pos[b + 2] - pos[a + 2])
                e2 = (pos[c] - pos[a], pos[c + 1] - pos[a + 1], pos[c + 2] - pos[a + 2])
                g = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                cn = [0.0, 0.0, 0.0]
                for k in range(3):
                    cn[k] = sum(inv_lin[k2][k] * (nrm[a + k2] + nrm[b + k2] + nrm[c + k2]) for k2 in range(3))
                if sum(g[k] * cn[k] for k in range(3)) < 0.0:
                    tri[t + 1], tri[t + 2] = tri[t + 2], tri[t + 1]
                    flipped += 1
            if sys.byteorder != 'little':
                nrm.byteswap()
                tri.byteswap()
                col.byteswap()
            for data, target, accessor in (
                    (nrm, 34962, {'componentType': 5126, 'count': count, 'type': 'VEC3'}),
                    (tri, 34963, {'componentType': 5125, 'count': len(tri), 'type': 'SCALAR'}),
                    (col, 34962, {'componentType': 5126, 'count': count, 'type': 'VEC4'})):
                new_views.append({'byteOffset': len(out), 'byteLength': len(data) * data.itemsize, 'target': target})
                out += data.tobytes()
                out += bytes((-len(out)) % 4)
                new_accessors.append(accessor)
            prim['extras'] = dict(prim.get('extras', {}), CanopyOriginal={'NORMAL': normal_id, 'indices': index_id},
                                  CanopyBaked=['NORMAL', 'COLOR_0'])
            prim['_canopy'] = (len(new_accessors) - 3, len(new_accessors) - 2, len(new_accessors) - 1)
            print(f"node {gltf['nodes'][node_id].get('name')}: {gltf['materials'][prim['material']]['name']}: "
                  f"{count} vertices, {flipped}/{len(tri) // 3} triangles rewound outward")
        if determinant3(lin) < 0.0:
            sys.exit(f"node {gltf['nodes'][node_id].get('name')} mirrors its mesh; winding cannot be kept")
        report.append({'Node': gltf['nodes'][node_id].get('name'), 'Centre': centre,
                       'HalfExtent': [(hi[k] - lo[k]) * 0.5 for k in range(3)], 'DeepestCm': deepest})
        print(f"crown {gltf['nodes'][node_id].get('name')}: centre {centre}, deepest leaf {deepest:.0f} cm below the top")
    leaves = [l for ls in crowns.values() for l in ls]

    # Drop the previous run's sidecar views/accessors (referenced by nothing once repointed below).
    buffer_id = len(buffers_json)
    gltf['buffers'] = buffers_json + [{'uri': sidecar, 'byteLength': len(out)}]
    for view in new_views:
        view['buffer'] = buffer_id
    view_base = len(gltf['bufferViews'])
    gltf['bufferViews'] += new_views
    acc_base = len(gltf['accessors'])
    for k, acc in enumerate(new_accessors):
        acc['bufferView'] = view_base + k
    gltf['accessors'] += new_accessors
    for prim, _, _ in leaves:
        normal_acc, index_acc, color_acc = prim.pop('_canopy')
        prim.pop('_canopy_node')
        prim['attributes']['NORMAL'] = acc_base + normal_acc
        prim['attributes']['COLOR_0'] = acc_base + color_acc
        prim['indices'] = acc_base + index_acc
    compact(gltf)
    gltf.setdefault('extras', {})['CanopyNormals'] = {
        'Tool': 'scripts/Content/foliage_canopy_normals.py', 'MaterialSuffix': args.material_suffix, 'Weight': w,
        'CellCm': args.cell_cm, 'UnitCm': args.unit_cm, 'DepthRangeCm': CANOPY_DEPTH_RANGE_CM, 'Crowns': report}
    with open(os.path.join(base, sidecar), 'wb') as f:
        f.write(out)
    with open(args.gltf, 'w') as f:
        json.dump(gltf, f, indent=2)
        f.write('\n')
    print(f"wrote {sidecar}")


def compact(gltf):
    """Removes accessors and buffer views nothing references (a previous run's), renumbering the rest."""
    used = set()
    for mesh in gltf['meshes']:
        for prim in mesh['primitives']:
            used.update(prim['attributes'].values())
            if 'indices' in prim:
                used.add(prim['indices'])
            used.update(prim.get('extras', {}).get('CanopyOriginal', {}).values())
    for skin in gltf.get('skins', []):
        if 'inverseBindMatrices' in skin:
            used.add(skin['inverseBindMatrices'])
    for anim in gltf.get('animations', []):
        for s in anim['samplers']:
            used.update((s['input'], s['output']))
    remap = {old: new for new, old in enumerate(sorted(used))}
    gltf['accessors'] = [gltf['accessors'][old] for old in sorted(used)]
    views = sorted({a['bufferView'] for a in gltf['accessors']} |
                   {img['bufferView'] for img in gltf.get('images', []) if 'bufferView' in img})
    vremap = {old: new for new, old in enumerate(views)}
    gltf['bufferViews'] = [gltf['bufferViews'][old] for old in views]
    for a in gltf['accessors']:
        a['bufferView'] = vremap[a['bufferView']]
    for img in gltf.get('images', []):
        if 'bufferView' in img:
            img['bufferView'] = vremap[img['bufferView']]
    for mesh in gltf['meshes']:
        for prim in mesh['primitives']:
            prim['attributes'] = {k: remap[v] for k, v in prim['attributes'].items()}
            if 'indices' in prim:
                prim['indices'] = remap[prim['indices']]
            original = prim.get('extras', {}).get('CanopyOriginal')
            if original:
                prim['extras']['CanopyOriginal'] = {k: remap[v] for k, v in original.items()}
    for skin in gltf.get('skins', []):
        if 'inverseBindMatrices' in skin:
            skin['inverseBindMatrices'] = remap[skin['inverseBindMatrices']]
    for anim in gltf.get('animations', []):
        for s in anim['samplers']:
            s['input'], s['output'] = remap[s['input']], remap[s['output']]


if __name__ == '__main__':
    main()
