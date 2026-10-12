# Round 08 — lens: scale (pair: r08 Demo m vs r04 final Demo m, both ours)
A and B are the same frame to the eye everywhere except the bottom-right body (~615,470): in A it is a narrow
two-ball column with a bead on its foot, in B a slightly taller pile with a broader top. The middle puffs,
the top-left body and the big lower-left body are pixel-alike.
Prediction (weak, one body to go on): B is r08 (the column lost its floored turrets). Scale lens: no
difference that reads as a change of size; neither has flat bases. Expect REFUSED-as-neutral.

## After reveal
MISPREDICTED (weakly): A is r08. Fresh bake (DDC miss, v0x1a); lumps 3759 -> 2263, yet Demo m is unchanged
except one body. REFUSED, code reverted to r04: the 1496 dropped turret/billow lumps were inside the bodies'
own envelopes, so they were not what reads as balls.
Finding (r05-r08, four shape/H refusals): what reads as balls is the STACK LUMPS THEMSELVES at the volume's
scale. Region 48 km / 256 = 188 m per voxel (log: "188 m per voxel"), lumpFloorKm = voxel; a small Demo body
(R ~0.3 km) is ~3 voxels wide, so any lump layout inside it is one or two voxel-scale blobs. No layout change
can make a flat-based puff out of 3 voxels. Next lever is architectural: modelling-volume resolution for
small bodies (finer region / second-tier volume), or the march's detail noise carving the base flat.
