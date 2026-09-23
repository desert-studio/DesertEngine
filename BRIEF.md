# E4 — viewport layout, and ten icons that are downloaded but wired to nothing

Read `.claude/AGENT_BRIEF_COMMON.md` first. Your tree:
`/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-E4`, branch `task/E4-viewports`.
Absolute paths ALWAYS.

## The owner's report

«несколько окон просмотра сцены с разных углов, поправить camera gizmos … а так же в целом
поправить все gizmos: красивые иконки как в UE, информативность итд!»

The camera gizmo itself is DONE — its frustum was drawn 2.5 cm long, a literal that predated the
project's decision that one world unit is one centimetre. That is merged. Two pieces remain.

## 1. The layout

`Scenes -> New Scene View` exists and several live SceneRenderers are now legal — the scene holds
its views and one ECS walk feeds N of them (merged yesterday as U9, read that commit). Six
renderer slots; a preview must be DESTROYED to give its slot back.

What is missing is the LAYOUT: UE gives four viewports in a grid with per-viewport camera
presets (perspective / top / front / side). Today each new view is a floating panel the user
arranges by hand.

Take the PATTERN, not the letter. Ask what the four-up grid actually buys — the answer is
"see the same object from fixed orthogonal directions at once", and how we spell that is ours to
choose. If a simpler mechanism answers it, take ours and write down why.

**Check before you build**: verify that `AddSceneView` now attaches to the CURRENT scene. It used
to create a brand-new empty Scene, which is why a second view rendered nothing. I believe U9 fixed
this; confirm it rather than assuming.

## 2. The icons

Ten Phosphor duotone SVGs live in `Editor/Resources/Icons/Gizmo/`, named by ROLE
(`light-point.svg`, `light-directional.svg`, `light-spot.svg`, `camera.svg`, `audio-source.svg`,
`trigger-volume.svg`, `spawn-point.svg`, `text.svg`, `transform-empty.svg`, `script.svg`), with
`LICENSE-MIT-Phosphor.txt` and a `NOTICE.md` recording why Phosphor over MDI. The SVG parser was
taught to fold `opacity` / `fill-opacity` into the fill alpha, without which duotone collapses to
one flat layer — `Tests/Engine/VectorImage` holds that with a negative control.

**Nothing is wired.** Gizmos still draw font glyphs. That is the job: make the renderer use these.

While you are there, "информативность": a gizmo should say WHICH light it is, not just that a
light exists. Colour from the light's own colour, a radius ring for point lights, a cone for
spots. Propose, measure, and refuse anything that costs more than it tells.

## Proof

The editor DOES run here — `scripts/MacOS/RunEditor.sh`, and `--scene X --shot out.png --camera
--look` renders unattended. **Render and LOOK before claiming a visual change works.** Panel UI is
verifiable; the common brief has the section. `--shot-frames 3` is FORBIDDEN (empty png below
frames-in-flight).

## Rules

Never run clang-tidy or clang-format. English commit messages. Own scratchpad subdirectory.
WIP-commit whenever you pause — an agent died mid-task yesterday and only its commits survived.
Never commit ThirdParty pointers. Build at `-j3`; two other agents share this machine.

If a claim above is false, say so. Briefs here have carried stale citations before.
