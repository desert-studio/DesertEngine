# Round 01 — lens: silhouette (not blind in practice: both images were seen while setting up the bar)
A/B not opened; the two candidates are known: the checker-floor Demo horizon and the UE stratocumulus sheet.
Prediction: ours LOSES on silhouette. The UE edge is a continuous soft surface that fades into wisps;
ours has heads (good, CLOUD-SHAPE-f) but every head's outline is broken into pixel-scale crumbs and loose
flecks float beside the bodies — the edge reads as popcorn, not as a surface. The fine erosion octave
(HF weight 0.5 at 4x) and Detail Strength 0.85 are cutting at a scale smaller than a head.
Biggest gap: sub-head grain on the outline. Change: Detail Strength 0.85 -> 0.65 (brief's first lever).

## After the change (seen, not blind): rounds/01/before_after_Demo_h.jpg (left 0.85, right 0.65)
Loose flecks mostly gone, crumbled outline now continuous lobes, heads still separated. Remaining: a fine pebbly
texture over every lit face (the 4x octave) — still not the bar's continuous surface. Showcase unchanged in character.
