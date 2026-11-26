#!/usr/bin/env python3
"""Desert Studio mark and lockup: the one source of their geometry.

Every shape is a list of primitives (box, half ring, leg) defined once below. The SVG paths and the
signed-distance field the PNGs are rasterised from are both derived from those primitives, so a number
changed here changes every output. The game's studio ident video is rendered from these same numbers
outside the engine repository.

Regenerate (from the repository root):  python scripts/Branding/studio_mark.py
Requires numpy and Pillow. Units: mark half-height = 1, glyph cap height = 1, y up.
"""
import argparse
import os

import numpy as np
from PIL import Image

DEFAULT_OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Editor', 'Resources', 'Branding', 'Studio')

# ---------------------------------------------------------------- mark: a D whose stem is cut from the bowl by a slit
MK_S = 0.40   # stroke
MK_G = 0.105  # the slit between stem and bowl
MK_L = 0.50   # flat arm length before the bowl's arc
MK_W = MK_S + MK_G + MK_L + 1.0
MK_X0 = -MK_W / 2            # stem left
MK_XB = MK_X0 + MK_S + MK_G  # bowl left end
MK_XC = MK_XB + MK_L         # bowl arc centre
MK_RI = 1.0 - MK_S           # bowl inner radius

# ---------------------------------------------------------------- wordmark: monoline geometric caps
GW = 0.150    # stroke
GH = GW / 2
WORD = 'DESERT STUDIO'
SPACE_W = 0.55

# lockup: mark (height 2) over the wordmark
CAP = 0.292   # cap height in mark units
GAP = 0.46    # mark bottom to cap top
TRACKING = 0.62
MARGIN = 1.3  # half extent of the mark's square canvas


def glyph(ch):
    """(advance width, primitives) of one cap; x from 0, baseline y = 0, cap y = 1.

    ('box', x0, y0, x1, y1); ('harc', cx, cy, rc, side) a ring of radius rc and stroke GW, the half on
    side 'R', 'L' or 'B'; ('ring', cx, cy, rc) a full ring; ('leg', ax, ay, bx, by) a stroke from a toward b
    cut at the baseline."""
    w, h = GW, GH
    if ch == 'I':
        return w, [('box', 0, 0, w, 1)]
    if ch == 'T':
        W = 0.70
        return W, [('box', 0, 1 - w, W, 1), ('box', W / 2 - h, 0, W / 2 + h, 1)]
    if ch == 'E':
        W = 0.60
        return W, [('box', 0, 0, w, 1), ('box', 0, 1 - w, W, 1), ('box', 0, 0.5 - h, W * 0.88, 0.5 + h), ('box', 0, 0, W, w)]
    if ch == 'D':
        xc = 0.30
        return xc + 0.5, [('box', 0, 0, w, 1), ('box', 0, 1 - w, xc, 1), ('box', 0, 0, xc, w), ('harc', xc, 0.5, 0.5 - h, 'R')]
    if ch == 'O':
        return 1.0, [('ring', 0.5, 0.5, 0.5 - h)]
    if ch == 'U':
        W = 0.76
        r = W / 2
        return W, [('box', 0, r, w, 1), ('box', W - w, r, W, 1), ('harc', r, r, r - h, 'B')]
    if ch == 'R':
        xc, yb = 0.34, 0.44
        rr = (1 - yb) / 2
        W = xc + rr
        return W, [('box', 0, 0, w, 1), ('box', 0, 1 - w, xc, 1), ('box', 0, yb, xc, yb + w),
                   ('harc', xc, yb + rr, rr - h, 'R'), ('leg', xc - 0.02, yb + h, W - h * 1.15, -0.2)]
    if ch == 'S':
        W = 0.64
        rc = (0.5 - h) / 2
        ro = rc + h
        xl, xr = ro, W - ro
        return W, [('box', xl, 1 - w, W, 1), ('harc', xl, 1 - h - rc, rc, 'L'), ('box', xl, 0.5 - h, xr, 0.5 + h),
                   ('harc', xr, h + rc, rc, 'R'), ('box', 0, 0, xr, w)]
    raise KeyError(ch)


def word_layout(tracking):
    """[(ch, x0, width)] in cap units, centred on 0, and the total width."""
    xs, x = [], 0.0
    for ch in WORD:
        wd = SPACE_W if ch == ' ' else glyph(ch)[0]
        xs.append((ch, x, wd))
        x += wd + tracking
    total = x - tracking
    return [(c, a - total / 2, wd) for c, a, wd in xs], total


# ---------------------------------------------------------------- signed distance (negative inside)
def sd_box(x, y, x0, y0, x1, y1):
    cx, cy, hx, hy = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    dx, dy = np.abs(x - cx) - hx, np.abs(y - cy) - hy
    return np.hypot(np.maximum(dx, 0), np.maximum(dy, 0)) + np.minimum(np.maximum(dx, dy), 0)


def sd_prim(x, y, p):
    kind = p[0]
    if kind == 'box':
        return sd_box(x, y, *p[1:])
    if kind in ('harc', 'ring'):
        cx, cy, rc = p[1:4]
        d = np.abs(np.hypot(x - cx, y - cy) - rc) - GH
        if kind == 'ring':
            return d
        side = p[4]
        if side == 'R':
            return np.maximum(d, cx - x)
        if side == 'L':
            return np.maximum(d, x - cx)
        return np.maximum(d, y - cy)
    if kind == 'leg':
        ax, ay, bx, by = p[1:]
        px, py, dx, dy = x - ax, y - ay, bx - ax, by - ay
        t = np.clip((px * dx + py * dy) / (dx * dx + dy * dy), 0, 1)
        return np.maximum(np.hypot(px - t * dx, py - t * dy) - GH, -y)
    raise KeyError(kind)


def sd_mark(x, y):
    stem = sd_box(x, y, MK_X0, -1, MK_X0 + MK_S, 1)
    outer = np.minimum(sd_box(x, y, MK_XB, -1, MK_XC, 1), np.hypot(x - MK_XC, y) - 1.0)
    inner = np.minimum(sd_box(x, y, MK_XB - 9, -MK_RI, MK_XC, MK_RI), np.hypot(x - MK_XC, y) - MK_RI)
    bowl = np.maximum(np.maximum(outer, -inner), MK_XB - x)
    return np.minimum(stem, bowl)


def sd_word(x, y, tracking):
    """x, y in cap units, baseline y = 0."""
    lay, _ = word_layout(tracking)
    d = np.full(np.broadcast(x, y).shape, 9.0, np.float32)
    for ch, x0, wd in lay:
        if ch == ' ':
            continue
        cols = np.where(((x > x0 - 0.2) & (x < x0 + wd + 0.2)).any(axis=0))[0]
        if cols.size == 0:
            continue
        sl = (slice(None), slice(cols[0], cols[-1] + 1))
        for p in glyph(ch)[1]:
            d[sl] = np.minimum(d[sl], sd_prim(x[sl] - x0, y[sl], p))
    return d


# ---------------------------------------------------------------- SVG (y down)
def N(v):
    return f'{v:.4f}'.rstrip('0').rstrip('.')


def svg_prim(p):
    kind = p[0]
    if kind == 'box':
        x0, y0, x1, y1 = p[1:]
        return f'M{N(x0)} {N(1 - y1)}H{N(x1)}V{N(1 - y0)}H{N(x0)}Z'
    if kind == 'ring':
        cx, cy, rc = p[1:]
        ro, ri, cy = rc + GH, rc - GH, 1 - cy
        return (f'M{N(cx - ro)} {N(cy)}A{N(ro)} {N(ro)} 0 1 1 {N(cx + ro)} {N(cy)}A{N(ro)} {N(ro)} 0 1 1 {N(cx - ro)} {N(cy)}Z'
                f'M{N(cx - ri)} {N(cy)}A{N(ri)} {N(ri)} 0 1 0 {N(cx + ri)} {N(cy)}A{N(ri)} {N(ri)} 0 1 0 {N(cx - ri)} {N(cy)}Z')
    if kind == 'harc':
        cx, cy, rc, side = p[1:]
        ro, ri, cy = rc + GH, rc - GH, 1 - cy
        if side == 'R':
            return f'M{N(cx)} {N(cy - ro)}A{N(ro)} {N(ro)} 0 0 1 {N(cx)} {N(cy + ro)}V{N(cy + ri)}A{N(ri)} {N(ri)} 0 0 0 {N(cx)} {N(cy - ri)}Z'
        if side == 'L':
            return f'M{N(cx)} {N(cy - ro)}A{N(ro)} {N(ro)} 0 0 0 {N(cx)} {N(cy + ro)}V{N(cy + ri)}A{N(ri)} {N(ri)} 0 0 1 {N(cx)} {N(cy - ri)}Z'
        return f'M{N(cx - ro)} {N(cy)}A{N(ro)} {N(ro)} 0 0 0 {N(cx + ro)} {N(cy)}H{N(cx + ri)}A{N(ri)} {N(ri)} 0 0 1 {N(cx - ri)} {N(cy)}Z'
    if kind == 'leg':
        ax, ay, bx, by = p[1:]
        dx, dy = bx - ax, by - ay
        L = (dx * dx + dy * dy) ** 0.5
        nx, ny = -dy / L * GH, dx / L * GH

        def base(px, py):  # where the side line through (px, py) meets the baseline
            return px + (0 - py) / dy * dx
        return (f'M{N(ax + nx)} {N(1 - (ay + ny))}L{N(base(ax + nx, ay + ny))} 1'
                f'L{N(base(ax - nx, ay - ny))} 1L{N(ax - nx)} {N(1 - (ay - ny))}Z')
    raise KeyError(kind)


def mark_svg(fill):
    stem = f'<rect x="{MK_X0:.4f}" y="-1" width="{MK_S:.4f}" height="2" fill="{fill}"/>'
    bowl = (f'<path fill="{fill}" d="M{MK_XB:.4f} -1 H{MK_XC:.4f} A1 1 0 0 1 {MK_XC:.4f} 1 H{MK_XB:.4f} '
            f'V{MK_RI:.4f} H{MK_XC:.4f} A{MK_RI:.4f} {MK_RI:.4f} 0 0 0 {MK_XC:.4f} {-MK_RI:.4f} H{MK_XB:.4f} Z"/>')
    return stem + bowl


def word_svg(fill):
    lay, total = word_layout(TRACKING)
    oy = 1 + GAP
    g = ''.join(f'<path transform="translate({N(x0 * CAP)} {N(oy)}) scale({N(CAP)})" '
                f'd="{"".join(svg_prim(p) for p in glyph(ch)[1])}"/>' for ch, x0, wd in lay if ch != ' ')
    return f'<g fill="{fill}">{g}</g>', total * CAP


# ---------------------------------------------------------------- PNG (alpha from the SDF, 4x supersampled)
def write_png(path, w, h, half_h, cy, word):
    S = 4
    W, H = w * S, h * S
    px = 2.0 * half_h / H
    xs = (np.arange(W, dtype=np.float32) + 0.5 - W / 2) * px
    ys = (H / 2 - (np.arange(H, dtype=np.float32) + 0.5)) * px
    X, Y = np.meshgrid(xs, ys)
    Y = Y + cy
    a = np.clip(0.5 - sd_mark(X, Y) / px, 0, 1)
    if word:
        sw = sd_word(X / CAP, (Y - (-1 - GAP - CAP)) / CAP, TRACKING)
        a = np.maximum(a, np.clip(0.5 - sw / (px / CAP), 0, 1))
    a = a.reshape(h, S, w, S).mean(axis=(1, 3))
    im = np.zeros((h, w, 4), np.uint8)
    im[..., :3] = 255
    im[..., 3] = (a * 255 + 0.5).astype(np.uint8)
    Image.fromarray(im).save(path, optimize=True)


def write_text(path, text):
    with open(path, 'w', newline='\n') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', default=DEFAULT_OUT, help='output directory (default: Editor/Resources/Branding/Studio)')
    out = os.path.normpath(ap.parse_args().out)
    os.makedirs(out, exist_ok=True)
    m = MARGIN
    for name, fill in (('DesertStudioMark', '#ffffff'), ('DesertStudioMarkBlack', '#0b0c0e')):
        write_text(f'{out}/{name}.svg', f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{N(-m)} {N(-m)} {N(2 * m)} {N(2 * m)}">'
                                        f'<title>Desert Studio</title>{mark_svg(fill)}</svg>\n')
    wsvg, ww = word_svg('#ffffff')
    vw = ww + 0.6
    hh = 2 * m + GAP + CAP
    write_text(f'{out}/DesertStudioLogo.svg', f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{N(-vw / 2)} {N(-m)} {N(vw)} {N(hh)}">'
                                              f'<title>Desert Studio</title>{mark_svg("#ffffff")}{wsvg}</svg>\n')
    write_png(f'{out}/DesertStudioMark.png', 1024, 1024, m, 0, False)
    write_png(f'{out}/DesertStudioLogo.png', int(round(1024 * vw / hh)), 1024, hh / 2, m - hh / 2, True)
    print(f'wrote 5 files to {out}')


if __name__ == '__main__':
    main()
