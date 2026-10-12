# Round 03 — lens: scale (A/B not opened; contents known from r02: Demo m after r02 vs UE_mid)
Prediction: ours loses on scale. Seen from below at 45 deg, our bodies' undersides are nearly as white as their
lit flanks, so each body reads as a small backlit cotton ball metres across — the eye reads a kilometre-sized
cumulus by the grey of light that had to cross it. The bar's thick parts go grey-blue in the folds and bellies.
Biggest gap: bases/undersides too bright for the body's depth. Hypothesis: the multi-scatter octaves see only a
quarter of the optical depth each (MultiScatterOcclusion 0.25), so higher orders flood the base.
Change: MultiScatterOcclusion 0.25 -> 0.4847 (the tooltip's own physical value: third octave on the diffusion length).

## After the change (seen): rounds/03/before_after.jpg (left 0.25, right 0.4847; top Demo m, bottom Showcase h)
REFUSED. The occlusion dims the WHOLE body — lit tops lose as much as the bellies — so the top-to-base gradient
barely grows and Showcase (the accepted control) turns overcast-grey. The base is not too bright because the
higher orders are too transparent; it is too bright RELATIVE to the top, which a uniform series factor cannot fix.
What would change the answer: a base term that depends on height-in-body / sun-path length (e.g. ambient from below
= ground bounce + horizon sky instead of the isotropic sky the column-above occlusion receives), measured as
top/base luminance ratio on Showcase h. Reverted to 0.25.
