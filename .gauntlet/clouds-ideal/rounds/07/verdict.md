# Round 07 — lens: light by rotation, judged on silhouette (the change is shape-only; said, not hidden)
Pair: ours r07 Demo m vs r04 final Demo m (both ours — the blind question is "which has flatter-based small bodies").
A: middle small bodies are taller-than-wide columns (bead at ~495,335 two lumps stacked, ~435,330 a pile);
   top-left body a narrow ball; bottom-right bodies small.
B: the middle small bodies are wider than tall (puffs ~440,315, ~510,340); top-left body broader and lower;
   still ROUND undersides, no common flat floor; big lower-left body the same in both.
Prediction: B is r07. B wins silhouette modestly: columns became puffs. Neither has a flat base.

## After reveal
MISPREDICTED: A is r07, B is r04. Capping the body's height by its width (aspect 0.5+0.7*size, DDC v0x19, fresh
bake 4138 ms) made small Demo bodies MORE columnar, not flatter. REFUSED, code reverted to r04.
Why (code read after the reveal): the stack is squashed but the crown's turrets and their billows are floored
at lumpFloorKm = max(0.5*chord, volume voxel 188 m) (CloudProceduralVolume.cpp:542). On a body of R ~0.3 km
the crown lump is ~0.15 km, so 3 turrets (0.55*crown) and their billows (0.55*turret) are all raised to 0.188 km
— as big as the body's own lumps — and they sit on the crown: a pile of equal balls. Squashing the stack only
made that pile a larger share of the body. Next lever (r08): a body whose turret would fall under the volume's
lump floor gets no turrets/billows (a small cumulus has none), instead of turrets floored up to body size.
