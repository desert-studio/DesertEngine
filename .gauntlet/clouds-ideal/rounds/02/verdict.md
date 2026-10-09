# Round 02 — lens: light
Opened A only. A is the reference (wide soft sheet, UE axis gizmo) — so B is ours (Demo m, r01 build).
Verdict: ours loses on light. A's bodies carry a LARGE gradient: bright sunlit faces, soft blue-grey only in the
folds between lobes, the transition over tens of pixels. Ours (seen in r01 shots) has a near-uniform grey-white
surface stippled with a micro-shadow at every pebble of the fine erosion octave: the shading is spent on grain,
so no face reads as lit and no fold as shaded.
Biggest gap: micro-shading from the 4x octave flattens the body's light. Change: CLOUD_DETAIL_HF_WEIGHT 0.5 -> 0.3.

## After the change (seen): rounds/02/before_after_Demo_h.jpg (left 0.5, right 0.3)
Lit faces now continuous; shade collects in the folds between heads — reads as cauliflower lobes, not pebbles.
Showcase unchanged (sheet). Remaining under this lens: bases not darker than flanks (open gap).
