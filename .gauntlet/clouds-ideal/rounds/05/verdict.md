# Round 05 — lens: grain (A/B not opened; extensions give the pair away again: B.png is a render, A.jpg a bar crop — said, not hidden)
Ours = Clouds_Demo m at HEAD 4be98e045 (r04 final); ref = UE_mid. Grain: ours holds — r01/r02 removed the pebbly
HF stipple, surfaces read as continuous billows; no sparkle. Prediction: B (ours) ties A on grain at this scale.
What ours LOSES on is not grain but shape (open_gaps r04, brief CLOUD-GAUNTLET-c): the congestus bodies are round
balls — undersides curve up into the flanks, no flat floor. Seen in the frame, not measured as grain.
Diagnosis from the code (CloudProceduralVolume.cpp CloudProceduralCutJoin): the base plane is a sharp intersection
(max of distances), but the altitude density curve MULTIPLIES the depth profile before the coverage remap, so the
iso-surface near the floor is depth * density = 1 - g: a body's edge (shallow depth) clears it only higher up the
ramp than its core — a bowl whose radius is the ramp height (0.2 x band: ~0.7 km on congestus, ~0.2 km on mediocris,
which is why Showcase reads flat and Demo does not).
Change (Nubis height gradient as a CAP, CSG intersection with the band): profile = min(depth, density).
Expected: Demo bodies gain a flat floor with a sharp-ish corner; Showcase nearly unchanged (ramp 0.2 km already small).
