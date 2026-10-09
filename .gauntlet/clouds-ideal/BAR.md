# Bar -- clouds-ideal

GOAL: Clouds_Demo/Showcase read as real cumulus: flat dark bases, cauliflower heads, soft wispy edges, no grain/sparkle/popcorn
BAR: .gauntlet/clouds-ideal/bar/UE_{horizon,mid,zenith}.jpg (UE5 VolumetricCloud viewport crops) — surface/edge texture only, NOT subject-matched (stratocumulus sheet vs our cumulus); see BAR.md
CAPTURE: /private/tmp/claude-501/CLOUD-SHAPE-f/shoot.sh <Clouds_Demo|Clouds_Showcase> .gauntlet/clouds-ideal/rounds/NN/shots

## Is this bar honest?

A bar must be OPENABLE and SUBJECT-MATCHED. Ours has failed the second test before:
`Docs/Clouds/UEReference/UE_mid.png` is a distant thin stratocumulus sheet, our
protocol sky is a near congestus deck, and R0 refused four shape instruments as
evidence for exactly that reason. A run against a mismatched bar industrialises a
comparison we already know is wrong. If the bar is not subject-matched, say so here
and say what it can and cannot decide.

## Answer (CLOUD-GAUNTLET, 2026-10-09)

NOT subject-matched. The bar crops (`bar/UE_{horizon,mid,zenith}.jpg`, from the owner's UE5
VolumetricCloud viewport screenshots, `Docs/Clouds/UEReference/UE_*.png`) show a stratocumulus
sheet/towers on UE's default m_SimpleVolumetricCloud. Our scenes are scattered fair-weather cumulus.

The bar CAN decide: surface/edge texture at the same apparent scale — soft, continuous, no
pixel-scale grain, no sparkle, no detached flecks; edges fade (wisp) rather than crumble.
The bar CANNOT decide: silhouette/lobe structure (flat bases, cauliflower heads), coverage,
body size. Those are judged against the owner's words ("flat dark bases, cauliflower heads")
and real cumulus photos in memory — written down in each verdict as such, not as "matches UE".
Showcase is the internal control: the owner accepts how it looks; a change that degrades it is regressed.

## Cumulus reference, written down (CLOUD-GAUNTLET-b, 2026-10-09) — the bar for base/top tone

No subject-matched image is in the repo (owner rule: external assets not committed), so the cumulus
standard is DESCRIBED here and every verdict on base/top tone is judged against this text, not the UE crops.
Fair-weather cumulus (Cu humilis/mediocris) seen from the ground in daylight, sun 30–60 deg up:
- BASE: flat, near-horizontal, all bodies at about one altitude; tone mid grey to grey-blue, clearly darker
  than any sunlit face — on a photo the base sits roughly 0.45–0.65 of the sunlit top's luminance, darker
  the thicker the body (a 1 km body's base is darker than a 200 m puff's).
- TOP/FLANKS facing the sun: near-white, the brightest thing in the sky; shaded flanks grey but still lighter
  than the base, because they see the open sky above and the base sees the dark ground below.
- The gradient runs with height inside the body: bottom third darkest, top third brightest; it is NOT a
  uniform grey over the whole body (r03's refusal: a global dim that greys tops fails this bar).
- Thin puffs (< ~200 m) stay bright all over — they have no dark base; only bodies with depth earn one.
What this can decide: the base-to-top luminance ratio per body (measured), and whether tops kept their white.
What it cannot decide: exact hue of the base (depends on ground albedo / sky), which is judged as "grey-blue, not brown".
