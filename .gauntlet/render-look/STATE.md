# render-look

Started 2026-10-19T16:24:31Z. Goal: an ordinary midday field reads AAA (TLOU2 raster bar) by renderer changes only; Projects/**/Content untouched

| round | lens | gap | outcome | note |
|---|---|---|---|---|
| 01 | depth | floor under the meadow lit as if no canopy; SSAO (Chapman hemisphere, range check) blind to thin blades | improved | SSAO -> GTAO port (UE PostProcessAmbientOcclusion.usf GTAO combined, defaults FalloffEnd 200, ThicknessBlend 0.5, 2 angles). Measured AO in the ellie lower half (diag, tonemapped 8-bit): old p1/p5/p25/p50 169/184/203/212, GTAO 165/178/200/213, GTAO thickness off 125/140/162/180. Visible: contact occlusion at wall/grass base and inside the carpet; the bright floor is NOT an AO-algorithm defect: diag d1 shows the floor sun-lit (shadow~1) - blade count (content) and sub-texel blade shadows. Kept. |
