# Round 04 — lens: coherence (A/B not opened; the file extensions give the pair away: A.png is a render, B.jpg a bar crop — said here rather than hidden)
Ours = Showcase h at HEAD (r02 state); ref = UE_horizon. Judged against BAR.md's written cumulus standard for tone,
the UE crop only for surface texture.
Prediction: ours loses on coherence of LIGHT across the body. Measured (basetop.py, eroded masks, linear luma):
Demo h/m base/top ≈ 1.04 / 0.87 area-weighted — the base is as bright as the top, bodies read as uniformly lit
cotton; Showcase 0.65 / 0.55 holds a gradient. The two scenes disagree because the ambient is isotropic
(full-sphere mean, CloudRaymarch.shader:607-611) and the only height-aware occluder is the column ABOVE the sample.
Biggest gap: no height-in-layer occlusion of the sky light toward the cloud bottom.
Change (UE pattern, VolumetricCloud "SkyLightCloudBottomOcclusion"): sky-light visibility
= mix(1 - BottomOcclusion, 1, heightFraction), BottomOcclusion 0.5 (UE default).
Expected: Demo base/top drops toward 0.6–0.7, tops' luma (max-top) within noise of 0.53.

## After the change (seen): rounds/04/before_after.jpg (top r02, bottom r04; Demo h, Demo m, Showcase h)
IMPROVED on Showcase, NEUTRAL on Demo. Showcase h biggest body: base 0.228 -> 0.186, top 0.465 -> 0.441 linear
(ratio 0.49 -> 0.42), Showcase m area-wt 0.55 -> 0.47; tops lose 5 %, bases 18 % — the gradient r03 could not make.
Repeat shot byte-identical (noise floor 0); built parameter path == shader experiment byte-for-byte (final/).
Demo m stays 0.87 -> 0.84: diagnostic shot with ambient OFF still gives base/top 0.94, so Demo's bright bellies are
NOT the sky light — they are the SHAPE (congestus bodies are round balls with no flat base) and the sun behind the
camera lighting the faces we see. Next gap, not this round: Cumulus_Congestus vertical profile (flat base).
Change kept as a real parameter: Sky Light Cloud Bottom Occlusion (material, Lighting, default 0.5 = UE).
