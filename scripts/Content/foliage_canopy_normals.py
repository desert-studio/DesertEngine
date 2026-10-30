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
# The source .bin is not touched: the new normals and indices go to a sidecar buffer <name>_canopy.bin, and the
# leaf primitives' NORMAL / indices accessors are repointed to it; the original accessor ids are kept in the
# primitive's extras, so a re-run bakes from the original data again. Pure Python (no numpy).
import argparse, json, math, os, struct, sys
from array import array

COMPONENT = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gltf')
    ap.add_argument('--material-suffix', default='_leaves')
    ap.add_argument('--weight', type=float, default=1.0)
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

    leaves = []
    for mesh in gltf['meshes']:
        for prim in mesh['primitives']:
            name = gltf['materials'][prim['material']]['name'] if 'material' in prim else ''
            if name.endswith(args.material_suffix):
                if prim.get('mode', 4) != 4:
                    sys.exit(f"{name}: only triangle lists are supported")
                original = prim.get('extras', {}).get('CanopyOriginal', {})
                leaves.append((prim, original.get('NORMAL', prim['attributes']['NORMAL']),
                               original.get('indices', prim['indices'])))
    if not leaves:
        sys.exit(f"no primitive whose material ends with '{args.material_suffix}'")

    positions = [read_accessor(gltf, buffers, p['attributes']['POSITION']) for p, _, _ in leaves]
    lo = [min(pos[k::3]) for pos in positions for k in range(3)]
    hi = [max(pos[k::3]) for pos in positions for k in range(3)]
    lo = [min(lo[k::3]) for k in range(3)]
    hi = [max(hi[k::3]) for k in range(3)]
    centre = [(lo[k] + hi[k]) * 0.5 for k in range(3)]
    inv_r2 = [1.0 / max((hi[k] - lo[k]) * 0.5, 1e-6) ** 2 for k in range(3)]
    w = min(max(args.weight, 0.0), 1.0)

    out = bytearray()
    new_views, new_accessors = [], []
    for (prim, normal_id, index_id), pos in zip(leaves, positions):
        own = read_accessor(gltf, buffers, normal_id)
        idx = read_accessor(gltf, buffers, index_id)
        count = len(pos) // 3
        nrm = array('f', bytes(len(own) * 4))
        for v in range(count):
            o = 3 * v
            # The crown ellipsoid's outward normal at the vertex (the gradient of its implicit function).
            cx = (pos[o] - centre[0]) * inv_r2[0]
            cy = (pos[o + 1] - centre[1]) * inv_r2[1]
            cz = (pos[o + 2] - centre[2]) * inv_r2[2]
            cl = math.sqrt(cx * cx + cy * cy + cz * cz) or 1.0
            cx, cy, cz = cx / cl, cy / cl, cz / cl
            # The card's own normal, turned to the canopy's side (its sign is the card's arbitrary winding).
            ox, oy, oz = own[o], own[o + 1], own[o + 2]
            if ox * cx + oy * cy + oz * cz < 0.0:
                ox, oy, oz = -ox, -oy, -oz
            nx, ny, nz = ox + (cx - ox) * w, oy + (cy - oy) * w, oz + (cz - oz) * w
            nl = math.sqrt(nx * nx + ny * ny + nz * nz)
            if nl < 1e-6:
                nx, ny, nz, nl = cx, cy, cz, 1.0
            nrm[o], nrm[o + 1], nrm[o + 2] = nx / nl, ny / nl, nz / nl
        tri = array('I', idx)
        flipped = 0
        for t in range(0, len(tri), 3):
            a, b, c = 3 * tri[t], 3 * tri[t + 1], 3 * tri[t + 2]
            e1 = (pos[b] - pos[a], pos[b + 1] - pos[a + 1], pos[b + 2] - pos[a + 2])
            e2 = (pos[c] - pos[a], pos[c + 1] - pos[a + 1], pos[c + 2] - pos[a + 2])
            g = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            s = sum(g[k] * (nrm[a + k] + nrm[b + k] + nrm[c + k]) for k in range(3))
            if s < 0.0:
                tri[t + 1], tri[t + 2] = tri[t + 2], tri[t + 1]
                flipped += 1
        if sys.byteorder != 'little':
            nrm.byteswap()
            tri.byteswap()
        for data, target, accessor in (
                (nrm, 34962, {'componentType': 5126, 'count': count, 'type': 'VEC3'}),
                (tri, 34963, {'componentType': 5125, 'count': len(tri), 'type': 'SCALAR'})):
            new_views.append({'byteOffset': len(out), 'byteLength': len(data) * data.itemsize, 'target': target})
            out += data.tobytes()
            out += bytes((-len(out)) % 4)
            new_accessors.append(accessor)
        prim['extras'] = dict(prim.get('extras', {}), CanopyOriginal={'NORMAL': normal_id, 'indices': index_id})
        prim['_canopy'] = (len(new_accessors) - 2, len(new_accessors) - 1)
        print(f"{gltf['materials'][prim['material']]['name']}: {count} vertices, {flipped}/{len(tri) // 3} "
              f"triangles rewound outward")

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
        normal_acc, index_acc = prim.pop('_canopy')
        prim['attributes']['NORMAL'] = acc_base + normal_acc
        prim['indices'] = acc_base + index_acc
    compact(gltf)
    gltf.setdefault('extras', {})['CanopyNormals'] = {
        'Tool': 'scripts/Content/foliage_canopy_normals.py', 'MaterialSuffix': args.material_suffix, 'Weight': w,
        'Centre': centre, 'HalfExtent': [(hi[k] - lo[k]) * 0.5 for k in range(3)]}
    with open(os.path.join(base, sidecar), 'wb') as f:
        f.write(out)
    with open(args.gltf, 'w') as f:
        json.dump(gltf, f, indent=2)
        f.write('\n')
    print(f"crown centre {centre}, half extent {[(hi[k] - lo[k]) * 0.5 for k in range(3)]}; wrote {sidecar}")


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
