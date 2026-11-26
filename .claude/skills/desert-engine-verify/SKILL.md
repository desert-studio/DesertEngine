---
name: desert-engine-verify
description: How a change is proven in DesertEngine — the .scn scenario and its frames, what a frame may and may not be believed for, numeric frame comparison and the noise floor, finding a mechanism by instrumenting and knocking out, and measuring performance on a shared machine. Use when writing a scenario, accepting a batch, comparing frames, hunting the cause of a visual defect, or reporting any timing.
---
# Proof in DesertEngine

The four most expensive defects of this engine (a tonemapper that reduced to the identity, a radiance five thousand times too large, a cloud shadow marching the far side of the planet, an empty sky at 50 % coverage) shipped built, checked and unseen. Each was a minute's work for anyone looking at a frame. So: what changes the screen is proven by a frame somebody looked at, next to the reference.

Who does what: an engineer writes the scenario and never runs the editor; the lead builds the batch once; one `acceptance` agent plays the scenarios, one editor at a time. Writing and running unit tests is off until the owner re-enables it.

## The scenario is the specification
`Scenarios/<Subsystem>/<CODE>.scn`, written from the design's `## Приёмка` and committed with the code. One step per line, `#` comments, the first comment is the title:

```
# WATER-FALLS: a fall has foam at its foot and mist above it
Проект     Projects/Desert/Desert.deproj --scene Content/Scenes/Falls.desce
Дано       <DesertCtl args>                 setup
Когда      <DesertCtl args>                 the action under test
Когда-файл touch <path>                     an edit made outside the editor (hot reload)
Тогда      fall_ui   the Water panel shows the fall's foam row          whole editor, interface included
Тогда-вид  fall_foot white foam at the foot, none on the calm pool      3D viewport only
```

- Played only by `python .claude/tools/play_scenario.py <TREE> <scenario.scn>`. No sleeps: every DesertCtl reply means the step finished and a frame with its result was presented. The first refused step stops the run and shoots `fail.png`.
- Output: `<scratch>/<CODE>/frames/scn-latest/` with every frame, `index.txt` (frame → what it must show), `result.txt`, `editor.log`.
- Every «Тогда [кадр]» of the design is a `Тогда` or `Тогда-вид` line with the same frame name; `studio.py merge` refuses a feature whose scenario lacks one.
- `Тогда` and `Тогда-вид` never substitute for one another: an interface claim needs the whole window, a pixel comparison needs the viewport.
- Take at least two frames per change: what it fixed and what it could have broken. They fail in opposite directions.
- A change to anything temporal (TAA, reprojection, accumulation, streaming) needs a frame after camera motion: under a fixed camera reprojection is exact by construction and proves nothing.
- macOS: the editor is started only through `scripts/MacOS/RunEditor.sh` (it exports the Vulkan loader environment); the scenario player does that itself.

## Believing a frame
1. Read `editor.log` first: VUID, `did not execute`, `refused`. Zero validation errors can mean nothing rendered.
2. Open the frame. A black or blank frame is FAIL, not "identical": two blank files compare equal. `limits.BLANK_FRAME_BYTES` is the size below which a frame is treated as blank.
3. Compare numerically before claiming any difference or any equality: `python scripts/Dev/frames_compare.py a.png b.png diff.png` prints the share of differing pixels, the max and mean delta and the bounding box. A "washed-out shadow" seen in a thumbnail grid was 0.013/255.
4. The noise floor is a property of the scene, not a constant: two runs of the same scenario on the same build first. Anything driven by the wall clock (particles) puts it above zero; a claim of "byte-identical" without a measured floor is not a claim.
5. A zero floor tells you where a change landed, never whether it is an improvement. You still look.
6. Discard the first render in a fresh worktree and the first render after a shader edit (the shader misses the cook cache and compiles during that load; measured: 100 % of pixels differed once and never again).
7. Frames from two trees are comparable only with the same `Editor/imgui.ini` (untracked; it sets the viewport size) and with default machine settings (`machine.json` overrides once faked a 3458-pixel regression). The log prints the trace resolution: check it is equal on both sides.
8. A GI or ray-tracing method override in machine settings is a ceiling, not a request: read the method the log reports before naming the frame after it.

## What is not proof
- "It builds." "The reviewer accepted." "The log is clean." "The number moved."
- A frame nobody opened; a frame without its reference beside it; a thumbnail grid.
- A diff over frames whose floor was not measured.
- One camera angle. Judge the angles that do not flatter.

## Finding the mechanism: instrument, knock out, then change
No defect in this project's history was found by reading code and guessing.
1. Render the suspect quantity as colour (sample count, the tier a ray ended in, a fade weight) or use an existing view mode. A frame that is entirely red is an argument no review produces.
2. Knock out one contributor at a time and measure the artefact each time; publish the table. Changing resolution or a period tells a screen-locked artefact from a world-locked one.
3. State the mechanism with numbers before changing anything: "the stride is 700 m where the structure is 391 m" is a finding; "the far field is noisy" is not.
4. Build the ground truth when you can (tiny steps, huge budget): it turns "better" into "17 % below the reference instead of 25 % above".
5. A hypothesis you disproved is a result: write it down where the next run will find it.

Most defects here were two individually correct values that disagreed (a noise tile against a layer thickness, a fade end against a view distance, a C++ mirror against the shader's formula). Ask what the two sides of the change are and state their agreement in code: a `static_assert`, a derived default, a logged warning that names the number (skill `desert-engine-dev`, «Footguns»). Two properties catch whole classes: a bound ("a step cannot scatter more light than its source") and a monotonicity ("more cloud lies above a low sample than a high one").

## Performance
- Before any timing: the PC is unlocked and the display is on (a locked machine drops the GPU to its lowest power state and every pass reads 1.5–7× slow), and the log's VRAM budget line is under budget (over budget, WDDM paging makes many passes 10–100× slow and looks like a cache regression).
- The machine is shared. Interleave A and B in one session and take the minimum of N, never the mean; give the spread. Absolute numbers taken twenty minutes apart are not comparable.
- A shader-only A/B needs no second binary: shaders are cooked at run time, swap the file (and discard the first run after the swap).
- A timing is reported with its configuration (`scripts/Dev/dev_config.txt`), resolution, scene and floor. A number without its noise floor is not a measurement.
- A refusal backed by such numbers is a finished task: `desert-engine-contract`, «A measured refusal counts as done».
