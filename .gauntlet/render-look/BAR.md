# Bar

Reference: `DesertEngine-scratch/FIELD-LOOK-REF/tlou2_farm_reference.jpg` (TLOU2, PS4 raster, no RT; 728x409 JPEG).

## What it can decide
- Render QUALITY of an outdoor field: depth of the grass carpet (occlusion at the base, lit tips), tonal range and
  grading (filmic), aerial perspective, material response of grass/wood, softness of contact.
- Owner 10-19: "do not change the scene - just render it well". So the bar judges how our renderer treats OUR
  content, not the content.

## What it cannot decide
- Subject: TLOU2 is overcast, dry wheat, a character, a white farmhouse. FIELD_Barn is sunny midday, green
  meadow, red barn. Hue, sky, sun direction, grass species and density are CONTENT and are not chased.
- Resolution/compression: the reference is a small JPEG; fine detail and noise are not comparable.
- So the blind A/B will always identify the reference by subject; a verdict judges the lens's render quality
  question on each image, not "which looks like TLOU2".
