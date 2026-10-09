# Round 06 — lens: silhouette (pair: r04 final Demo m vs UE_mid; formats differ so the pair is guessable — said, not hidden)
A: broad flattened masses with soft irregular outlines, small bodies are wider than tall, edges dissolve.
B: isolated small bodies are ROUND BALLS (some two balls stacked, a bead column), round undersides floating
clear of any common floor; only the big body bottom-left has a broadening skirt.
Prediction: B is ours. A wins silhouette: B's small bodies have no floor at all.
Diagnosis (code, before the change): the altitude density H (Profile.Density, smoothstep over the lowest
0.2 of the BAND = 0.72 km on congestus) is read on the type's band, not on the body. A small body is
~1.1 km tall (fullness 0.15+0.85*size), so H erases its lower ~0.35 km: the base lumps never clear the
remap and the visible body is the upper lumps — balls hanging over the condensation level.
r05's min() kept H at zero on the base, which is why it changed nothing.
Change: H read over the body's own height (base -> crown of its highest lump). Expect small Demo bodies
to gain a broad flat floor near the base; Showcase (big bodies, band-sized) nearly unchanged.

## After the change (seen): rounds/06/shots/Clouds_{Demo,Showcase}_{h,m}.jpg
REFUSED. Fresh bake (DDC bucket entries written 19:07 by the Demo h shot, v0x18; m shots then hit it) and
Demo m is visually identical to r04: same balls, same bead column. H read per body changed nothing visible,
exactly as r05's min() did. Two H-side changes now refuted: the rounding is NOT the altitude density.
What remains is the stack geometry itself: a small body is a 6-lump column ~1.1 km tall of lumps ~0.2 km
across (fullness floor EdgeTopFraction 0.15 * 3.6 km band = 0.54 km min height regardless of width, lump
spacing travel*t^1.7 leaves gaps between upper lumps), so it reads as stacked balls. Reverted to r04 code.
