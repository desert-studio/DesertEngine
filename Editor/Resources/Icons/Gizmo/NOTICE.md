# Gizmo icons — Phosphor Icons, duotone weight

Ten SVGs under `Editor/Resources/Icons/Gizmo/`, taken from
[phosphor-icons/core](https://github.com/phosphor-icons/core), **MIT © Phosphor Icons**
(full text beside this file).

## Why this set, measured rather than preferred

The owner asked for better-looking gizmo icons and left the choice to the lead. Four permissively
licensed sets were compared against what a viewport billboard actually needs — legible at ~30 px,
over an arbitrary scene colour, in a 3D view:

| set | licence | verdict |
|---|---|---|
| **Phosphor** | MIT | **chosen** — 6 weights including **duotone**, which is the one thing that makes an icon read as an object rather than a symbol |
| Tabler | MIT | 6 220 icons but only 1 054 filled; single tone |
| Lucide | MIT | **out**: no filled variant at all — an outline stroke disappears over a busy scene |
| Material Symbols | Apache 2.0 | same house style as the MDI font already in use, which is what we are moving away from |

**Duotone is the deciding property, and this engine can carry it natively.** `Vector/IconBake.cpp`
bakes one SDF layer per colour run and each layer keeps its own RGBA, so a two-tone SVG stays
**vector-crisp at any size** — no sprite atlas, which would have gone soft on a large gizmo.

## What had to be fixed to make it work

Phosphor's duotone separates its tones by `opacity="0.2"`, not by colour — both paths are
`currentColor`. `VectorImage.cpp` **did not parse `opacity` at all**, so the two runs compared equal,
`IconBake` collapsed them into one layer, and the faint backing shape would have drawn at full
strength: a blob. Opacity is now folded into the fill's alpha, pinned by
`VectorImageParse.OpacitySeparatesTheTwoRunsOfADuotoneIcon` and its negative control.

## Names are by ROLE, not by picture

`light-point.svg`, not `lightbulb.svg`. The file says what it marks, so swapping the artwork later is
a file replacement rather than a rename through every call site. It also records the mapping the old
font glyphs got wrong — the camera was a **movie** camera, the trigger volume a map marker.
