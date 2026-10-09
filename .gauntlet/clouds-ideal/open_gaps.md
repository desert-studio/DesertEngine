# Noticed, not this round

One gap per round. Everything else waits here with the round it was seen in.
- r01: fine pebbly grain on lit faces (HF octave) — next lever CLOUD_DETAIL_HF_WEIGHT 0.5->0.3
- r01: bases on Demo are not flat/dark — bodies are grey all over, base reads as a rounded underside
- r01: Showcase z empty sky (no body overhead) — coherence lens cannot be judged there
- r03: base/top gradient — refused uniform lever (MultiScatterOcclusion); needs a directional ambient (sky from above vs ground bounce from below) — CloudRaymarch.shader:602-611 ambientRadiance, :744-783 occlusion
- r04: Demo (Cumulus_Congestus) bodies are round balls — no flat base, bellies lit by the sun behind camera; ambient OFF still gives base/top 0.94. Lever: congestus type's vertical profile (bottom rounding/sharp base), assets:Clouds/Types/Cumulus_Congestus.decloudtype
- r04: ground bounce from below (UE Ground Albedo) not ported — only matters once bases are flat
- r05: congestus round undersides NOT from the density ramp (refused r05). Next lever: small-body shape — CloudProceduralVolume.cpp:1406-1411 fullness/stack for size<1, lump aspect, base lump centred on base (:1513, :1577-1585); compare a single-lump body from below
- r06: H per body refused (2nd H-side refusal). Next lever: small-body stack geometry — body height should scale with its width (CloudProceduralVolume.cpp:1406-1411 fullness floor EdgeTopFraction keeps a 0.6 km body 1.15 km tall), and fewer/wider lumps for size<1 (:1444-1494 stackCount=6 fixed, kBlobsPerCluster :132)
- r07: body height capped by width (aspect 0.5+0.7*size) refused — small bodies MORE columnar
- r08: unresolvable turrets/billows dropped refused — lumps 3759->2263, frame unchanged. Small bodies (~0.6 km) are ~3 voxels of the 188 m modelling volume (48 km/256): the balls ARE the voxel-scale stack lumps. Next lever is volume resolution for small bodies (region/Width, second tier) — architectural, not a layout constant. Ground bounce (UE Ground Albedo) still not started.
