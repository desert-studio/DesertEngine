---
name: desert-engine-verify
description: >
  How to actually VERIFY a change in DesertEngine — render a frame and look at it, run every
  test suite rather than the ones whose name matches, and test the RELATION between two things
  rather than each thing alone. Use this before claiming a rendering, shader, material, sky,
  cloud, lighting or post-processing change works; before committing anything that touches a
  reflected component or a shader's bindings; and whenever tempted to report "builds and tests
  pass" as evidence that a picture is right. Encodes what the project's most expensive defects
  had in common.
---

# Verifying a change in DesertEngine

The engine's four most expensive defects — a tonemapper that reduced algebraically to the
identity, a radiance five thousand times too large, a cloud shadow map marching the antipodal
side of the planet, and an empty sky at 50% coverage — all shipped **built, tested, and unseen**.
None was catchable by a unit test. Each was a minute's work for anyone looking at a frame.

This skill is the counter-measure. It is short on purpose.

## 1. Render it and look

**The editor runs on this machine.** (A project memory said otherwise for a long time; that was
wrong and it cost a lot.) Vulkan works through MoltenVK, and the engine can render a scene to a
PNG with no human present:

```bash
# NOT optional. RunEditor.sh exports these, so anyone who has only ever launched through the script
# will not know they exist -- and without them glfwVulkanSupported() is false and the binary dies at
# VulkanContext.cpp:50 before one frame, which reads as a broken build rather than a bare environment.
B=$(brew --prefix)
export VK_ICD_FILENAMES="$B/etc/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$B/share/vulkan/explicit_layer.d"
export DYLD_FALLBACK_LIBRARY_PATH="$B/lib"

cd Editor && ../build/Bin/Debug/Editor --project Desert.deproj \
    --scene Resources/Assets/Scenes/Clouds_Protocol.desce \
    --shot /tmp/out.png --shot-frames 90 \
    --camera 0,200,0 --look 0,0.9,-1
```

- `--camera x,y,z` world units (1 unit = 1 cm), `--look x,y,z` a direction, need not be normalized.
- **The scenes are FLAT under `Resources/Assets/Scenes/`** — there is no `Clouds/` subdirectory, and the
  only subdirectory is `Autosave/`. This example cited `Scenes/Clouds/Clouds_PartlyCloudy.desce` for a
  long time and no such file has ever existed; anyone copying it got Р5's refusal (`--scene ... does not
  exist; refusing to capture a different scene under that name`) and had to go looking. `find
  Resources/Assets/Scenes -name '*.desce'` is the reliable way to pick one.
- **`--shot-frames` matters.** Volumetric clouds accumulate over ~10 frames, so a shot taken on
  frame 1 is a picture of the dither. 90 is a safe default.
- **NEVER 3, AND THIS DOCUMENT USED TO TELL YOU TO USE 3.** Below the engine's frames-in-flight the
  readback beats the first present, and `--shot` writes a **fully blank PNG** — mean 0, min 0, max 0.
  A blank file is byte-identical to another blank file, so a before/after pixel diff over two of them
  reports `IDENTICAL` and the check silently passes on nothing at all. One task got six IDENTICAL
  results that way and only caught it by opening the images. Laddered on a non-cloud scene: **5, 8,
  12, 20 and 30 are all byte-identical to the 90-frame shot; only 3 is empty.** So use 5 as the low
  end when you want an early frame, never 3 — and whatever count you use, **look at one of the
  images, or assert a non-zero mean, before believing a diff over them.** (The advice to shoot at 3
  came from a real defect that 90 frames hid — see the "convergence window is also a masking window"
  note. The defect was real; the number was wrong.)
- It segfaults during teardown afterwards — a known shutdown bug. The PNG is already written.
- Interactive: `./scripts/MacOS/RunEditor.sh Debug` (sets the ICD, layer path, `DYLD_FALLBACK_LIBRARY_PATH`
  and `cd`s to `Editor/`, which is what makes `Resources/...` resolve).

### `--shot` CANNOT PHOTOGRAPH THE INTERFACE, and for a whole programme nobody knew

`--shot` reads `Scene::GetFinalImage()` — the scene's offscreen image. ImGui is drawn in the
**swapchain** pass. So **no frame in this engine's history has ever contained a pixel of UI**, and
the `--open-menu` flag promised in its own comment that a shot would show the open menu — a promise
its own path could never keep. One task shipped two items with no frame because of it.

**Use the control channel** (`--control-socket <path>`, client `Tools/DesertCtl`). It runs the
command palette's own commands — so anything a human can do, it can do — and it takes **two
distinct shots that never substitute for one another**: `shot.window` (the whole editor, interface
included) and `shot.viewport` (the scene, no UI). It also reads state back — selection, open
documents, renderer slots in use, log tail with severity counts — so a picture and a number can be
put side by side and made to agree.

Ordering is a property of the protocol, not a coincidence: the reply is released only after a frame
that was *presented* and on whose entry no deferred work from that command remained. "Run the
command, then screenshot" cannot photograph the state before the command.

Synthetic input still does *not* work (`osascript`/System Events has no assistive access) — the
channel is what replaces it, not `screencapture`. Capturing the window from outside
(`CGWindowListCopyWindowInfo` + `screencapture -x -o -l<id>`) still works and is still the fallback
when the channel is not up; if you use it, match on `kCGWindowOwnerPID` — other editors run on this
machine, and a naive capture already cost one developer an hour spent reading someone else's window.

**Take at least two frames: what the change fixed, and what it could have broken.** These fail in
opposite directions and one alone is not evidence.

### For anything in the sky: shoot the WHOLE DOME, not six patches of it

```bash
scripts/MacOS/DomeSweep.sh Resources/Assets/Scenes/Clouds_Protocol.desce /tmp/dome
# -> /tmp/dome/Clouds_Protocol_dome.png
#    40 tiles: 8 azimuths x 5 elevations (5/25/45/65/85 deg), the angles in the filename AND burnt
#    into every tile. ~25 min at 90 frames. --elevations / --azimuths / --frames cut it down.
```

**This section used to prescribe six look directions — three elevations times two azimuths — and six
rays is a SAMPLE.** Everything it said about why one axis of coverage is not enough is still true and
is kept below; what changed on 2026-08-28 is that covering both axes is no longer a discipline
somebody has to remember. One command sweeps them and hands back one picture.

The two defects the six points were introduced to catch, neither of which they caught:

- The first frame ever taken sunward at high elevation showed **hard full-width horizontal bands**
  cutting through both sky and cloud, deterministic and reproducible to three decimals, needing BOTH
  conditions at once — sunward azimuth AND high elevation. It had been there, unseen, the whole time
  (`Docs/Clouds/REVIEW_622a01a6.md` Ц9). One axis of coverage hid it completely.
- A ten-merge cloud and sky programme was verified almost entirely from the horizon, and the owner
  found two defects by simply looking up: a zenith that was empty above ~20 degrees, and vertical
  streaking that cut every cloud at mid elevation. **The horizon is the most forgiving angle in the
  sky** — a grazing ray crosses dozens of weather cells and hundreds of samples, so it hides both
  sparsity and per-ray failures. The mid angle is where a player actually looks and where these
  defects live.

And both are ARBITRARY IN AZIMUTH. Nothing about a lattice of weather cells, a periodic noise or a
placement seed makes a defect appear along `-Z` rather than at 135 degrees; the bands were found at
one azimuth because that was the azimuth someone happened to shoot. Two directions out of a circle is
a one-in-four chance of standing in the right place, and eight cost the same afternoon.

Reading the sheet:

- **Azimuth 0 is `-Z`, azimuth 180 is `+Z`** — the sun's axis in the cloud scenes, so the old six
  points are COLUMNS of this dome and not a different set of rays. `AZ 000 / EL 45` and
  `--look 0,1,-1` are the same ray, and a number measured under the old protocol is comparable with a
  tile of the new one.
- Row 1 is the horizon and the last row the zenith, so the sheet reads bottom-up like the sky does.
- **The label and the ray cannot drift apart.** `DomeSweep.sh` computes no angles at all: it asks
  `DomeSheet --plan` for the `--look` vector, the file stem and the burnt-in label *together*, and
  `Desert/Tests/Tools/DomeSheetLayout` asserts the round trip label -> angles -> vector. A sheet that
  mislabels a tile is worse than no sheet, because it still looks like evidence — the same
  two-things-that-must-agree shape as §4.
- **The log prints the scene's NAME, not its path** — `Loading scene: Clouds_Protocol` comes from the
  `SceneName` field inside the file. Two different `.desce` copied from one original are therefore
  *indistinguishable in the log*, which is exactly what an A/B built by copying a scene and editing one
  field produces. Grepping for the filename finds nothing and reads as a mismatch; grepping for the name
  passes on the wrong file. **Give each variant a distinct `SceneName` when you build it**, then the
  check means something. Found on 2026-08-31 while re-measuring a disputed control: the integrator's own
  grep reported a mismatch that was not one, and the same blind spot would have hidden a real one.
- Every tile is checked against **its own log** before it reaches the sheet, and a sweep that lost one
  refuses to assemble rather than producing a sheet with a hole in it.
- **It is 1/4 resolution.** It says WHERE to look; the full-size tile (`--keep-tiles`) is what you
  then look at, and pixel diffs are still taken on full frames.
- **Commit the SHEET, not the tiles.** `Docs/Clouds/Shots` is 563 MB against a 958 MB packed history —
  the frames are three fifths of everything this repository has ever stored. (An earlier note here said
  "508 MB of 5.6 GB"; the 5.6 came from measuring `.git` while a dozen agent worktrees each held a full
  370 MB copy of the submodules, which is transient and not history at all.)
  Forty 1.2 MB frames per sweep is not a thing to keep; the tiles are deleted unless asked for, and
  the two or three that carry the argument are what get committed beside the sheet.

Sweep at `--frames 3` as well when the change touches per-frame-in-flight state: the convergence
window is also a masking window, and one such defect was live in `dev` and invisible at 90.

### The camera can move, and static frames prove nothing about the temporal stage

```
--camera-to x,y,z   --look-to x,y,z   --shot-sequence <dir>   --shot-every N
```

Position lerps, aim slerps, and giving one endpoint holds the other still. Under a **fixed** camera
the temporal resolve's reprojection is exact by construction, so a static shot says nothing about
disocclusion, the neighbourhood clamp under translation, or the Ultra tier's checkerboard. If your
change touches anything temporal, move the camera. A translation under a cloud deck is also the
strongest test of whether a layer reads as sitting at altitude.

### The noise floor of `--shot` is usually ZERO — but it is a property of the SCENE, so measure it

**Corrected 2026-08-19.** This section used to say cloud and sky scenes are not byte-reproducible
run to run, and prescribed freezing animation and measuring a noise floor before any comparison.
That is wrong for the headless `--shot` path. Two independent measurements on the same day:

- the same command run twice, `Clouds_Demo` sunward at 42°, 1103x668 — **0 differing bytes** of
  2 947 884;
- a developer's own repeat at 1280x766 — **0 differing pixels** of 980 480.

**Corrected again 2026-08-28, and this one is the important direction.** The sentence that stood
here — "whatever the interactive editor does with a wall-clock timestep, `--shot-frames` advances the
same way every run" — is **false for any scene with grass**. Grass sway was driven by
`steady_clock::now()` (`SceneRenderer.cpp` → `GrassUB.Wind.w`), not by the frame counter, so
`Terrain_Grass` differed by **8.94 % of pixels, max delta 50/255, between two runs of the UNMODIFIED
binary**. With grass off, the same scene's floor was exactly 0 bytes. The developer who found it was
about to claim "these two frames are byte-identical" as the load-bearing evidence of a migration —
which is exactly the claim a non-zero floor destroys.

**That particular non-zero floor is gone, and the rule it taught is not.** Г25 removed the procedural
grass generator (grass is a mesh asset from now on), and with it the only wall-clock input in the
frame: re-measured 2026-09-14 on the same scene and camera at 715x784, `Terrain_Grass` is **0
differing pixels of 560 560 between two runs**, down from 14.25 % (max 184/255) measured on the same
machine minutes earlier against the pre-cut binary. **Do not read that as "the floor is zero again".**
Nothing structural stops the next renderer from reaching for `steady_clock` — particles already do —
and the cost of finding out is one run.

So the rule is not "the floor is zero"; it is **"the floor is a property of the scene and the repeat
shot costs one run"**. Take it. A wall-clock input anywhere in the frame puts it above zero. Then:

- **Diff the pixels.** "18.4% of pixels changed, max delta 1/255" and "1.3% changed, max delta 14/255"
  are two different findings about two different fixes, and that separation was only available
  because the floor is zero. It is how the RD task proved the phase change was everywhere-but-tiny
  while the guide change was rare-but-large.
- **You still have to look.** A zero floor tells you WHERE a change landed, never whether it is an
  improvement. The same task measured a fix as live and correct while the artefact it was expected
  to remove stayed put — the numbers said "this changed 1.3% of pixels on the silhouettes", and only
  the frame said "the speckle is still there".
- **Do not claim a floor you did not measure.** Take the repeat shot; it costs one run.

## 2. What is NOT verification

Do not offer these as evidence that a picture is right:

> "builds cleanly" · "tests pass" · "the logic is correct" · "should work" · "matches the paper"

All four were true of the tonemapper that did nothing. If a change touches what appears on screen
and the report has no frame in it, the change is unverified — say so plainly rather than dressing
it up.

## 3. Run every suite, not the matching one

Test `*.make` files **do not exist** until premake is re-run with the `CI` variable set — the test
projects are only generated then:

**AND THE SWEEP MUST FAIL LOUDLY WHEN A SUITE DOES NOT BUILD.** The recipe below used to read
`make -f X.make …; if [ -x binary ]; then run it; fi` — so a suite whose SOURCE stopped compiling kept
being reported green, because the previous run's binary was still lying in `build/Bin/Tests/Debug`. It
happened for real: `ProjectFormat` had not compiled since the optional-version change landed, and two
consecutive sweeps of mine certified 138 green over a stale executable. Check the build's exit status
and count BUILD-FAIL separately from FAIL — a suite that cannot be built is not a suite that passed.
(Third face of `mutation-checks-leave-stale-objects`: stale `.o`, stale tool binary, stale TEST binary.)

**HOLD THE BUILD LOCK PER TARGET, NEVER AROUND THE WHOLE SWEEP.** When several agents share this
machine they serialise through `/tmp/desert_build.lock` via `BuildSlot.sh`. Wrapping the entire
148-suite loop in ONE `BuildSlot.sh` call holds that lock for **hours**, and every other agent waits
behind it — one sweep measured at two hours for exactly this reason. Wrap each `make` invocation, or
take the lock only for the library build at the top and let the per-suite builds contend normally.
And do **not** remove a stale lock file to unblock yourself: it belongs to a live process, and
deleting it oversubscribes the machine for everyone. If a lock looks dead, say so rather than clear
it — both of these happened on 2026-09-07, one costing hours and one costing a window of
oversubscription.

**Build `Common`, `Desert`, `Editor` and `Runtime` FIRST.** The per-suite makefiles do not rebuild the
libraries they link, so a sweep run straight after a commit tests yesterday's `libCommon.a` — and since
`Version.gen.hpp` is generated by the `Common` project, `BuildVersion` goes red for the honest reason
that the binary predates the commit you are sitting on. That is the test working; it is the recipe that
was wrong. (Same family as the stale `.o` and the stale tool binary in `mutation-checks-leave-stale-objects`.)

```bash
CI=true premake5 gmake
# The tool half of the skip list is DERIVED, not typed. Hand-maintaining it drifted twice in opposite
# directions: once it named three real suites as libraries and skipped 35 tests for a whole programme
# (§7), once it fell four tools behind and every sweep built them to report NO-BINARY. `ls Tools`
# cannot drift, because adding a tool is what creates the directory.
TOOLS="|$(ls Tools | tr '\n' '|')"
make -j8 Editor config=debug   # the four libraries, and the third-party archives the suites link
ran=0
# AND THE SUITE LIST COMES FROM PREMAKE, NOT FROM `*.make`. Globbing the makefiles reports a suite
# that NO LONGER EXISTS as BUILD-FAIL, forever: `premake5 gmake` writes makefiles but never deletes
# one whose project is gone, so a renamed or removed suite leaves an orphan behind on every machine
# and in every worktree. Measured 2026-09-15 — Ю16 moved PreviewSlotBudget to RendererSlotBudget and
# the very next sweep reported `BUILD-FAIL PreviewSlotBudget`, which reads exactly like the
# integration defect the sweep is there to catch. The root `Makefile` carries premake's own
# `PROJECTS :=` line and cannot contain a project premake does not know, so it is the honest source.
# (Do NOT try to spot orphans by mtime: premake rewrites only the files that changed, so almost every
# makefile is older than the newest one and the test says nothing. That was tried first.)
PROJECTS=$(sed -n 's/^PROJECTS := //p' Makefile | tr ' ' '\n')
for t in $PROJECTS; do
  case "$t" in Desert|Common|Editor|Runtime|GLFW|ImGui*|imgui-node-editor|yaml-cpp|Jolt|Lua|Optick|MeshOptimizer|Dlib|ReflectCpp|Assimp|BuildAllTests|RunAllTests) continue;; esac
  case "$TOOLS" in *"|$t|"*) continue;; esac
  if ! make -f "$t.make" config=debug -j8 >/dev/null 2>&1; then echo "BUILD-FAIL $t"; continue; fi
  if [ ! -x "build/Bin/Tests/Debug/$t" ]; then echo "NO-BINARY $t"; continue; fi
  ran=$((ran+1))
  # Match gtest's OWN bracketed marker, and check the exit code TOO. A bare `grep FAILED` was here
  # until 2026-09-08 and it reported `PackagedMount` red while all 11 of its tests were green: the
  # suite deliberately exercises a failed read, and Ф4's unwrap reporter prints "A FAILED result was
  # unwrapped ...", which the bare grep matched. Any suite that exercises a refusal path can do this.
  # The exit code is the other half, because a suite can crash AFTER printing "[  PASSED  ]" —
  # six cloud suites once did exactly that.
  out=$( ./build/Bin/Tests/Debug/$t 2>/dev/null ); rc=$?
  case "$out" in *"[  FAILED  ]"*) echo "FAIL $t"; continue;; esac
  [ "$rc" -ne 0 ] && echo "FAIL $t (exited $rc after reporting a pass)"
done
echo "$ran suites ran"
```

**Read all four outcomes, and quote the count.** `BUILD-FAIL` and `NO-BINARY` are the two ways a suite
disappears while the sweep still looks green, and the previous version of this block had neither — it
threw the build's exit status away and then guarded the run with `[ -x … ]`, which is the exact shape
the paragraph above condemns. It stayed that way for a whole session **inside the document that names
the defect**, because prose was corrected and the copy-pasted line beneath it was not. The count is
what makes a silent shrinkage visible: 138 one day and 121 the next is a finding, and no per-line
output ever says it.

`NO-BINARY` means the loop built a name that is not a test suite. With the tool half of the list
derived, it should be **empty**; every line in it is either a new tool that lives outside `Tools/` or a
suite whose binary the makefile no longer produces, and both need reading rather than tolerating.

**A Release sweep needs the libraries built first.** `Common`, `Optick`, `ReflectCpp` and
`MeshOptimizer` are skipped by the loop above but linked by the suites, and in a fresh tree Release has
none of them — so all 88 report `BUILD-FAIL` for a reason that has nothing to do with the change under
test. Build those four at `config=release` before the loop, or read the failure as your own.

Three suites fire from changes nowhere near their name. Expect them and fix them with the truth,
not by adjusting the number until it passes:

| suite | fires when |
|---|---|
| `ComponentReflection` | any `PROPERTY` added to a reflected component — it pins an exact field census and per-category counts |
| `SettingConsumers` | same — every field must name the file that reads it or the task that owes a reader |
| `ShaderCacheKey` | a shader gains or loses a binding — and note it **compiles the shader with shaderc and reflects the SPIR-V**, so GLSL edits *are* compile-checked |

Shader maths in `Editor/Resources/Shaders/Common/*.glslh` is compiled **as C++** by the test
references, so a shader edit can break tests without touching a single `.cpp`.

## 4. Test the relation, not the function

The defect taxonomy of this project, from three audits and a render session:

| defect | what actually disagreed |
|---|---|
| flat slabs instead of clouds | noise tile 35 km vs layer thickness 3.5 km |
| empty zenith | weather-map scale vs what a ground camera sees |
| no aerial perspective | ray's shell entry vs the cloud's own distance |
| a preset clipped by a ring | `HorizonFadeEnd` vs `MaxViewDistance` |
| a seam at 30 km | shadow map vs cone march |
| white blown-out sky | tonemapper white point vs the operator it feeds |
| grey clouds | a C++ mirror of a gradient vs the shader's own formula |
| authored tints never loading | preset table vs the saved scenes |
| the march made ZERO net progress | the skip's coarse tier judged by cheap density, its fine tier by density AFTER erosion |
| horizon shredded into torn paper | the same two tiers, again — a fine excursion inside real cloud counted erosion holes and jumped 2.8 km |
| a woven cross-hatch on cirrus | the rewind floor got the DITHERED start instead of the shell entry, deleting a slab off every near face |
| clouds looked close to the ground | cell width vs layer thickness — the fair-weather family was authored with cumulonimbus proportions |
| a far quad not drawn at all | a float depth buffer vs a standard-Z distribution that throws its precision away |

Four of those are the **same disagreement found four times**, which is the strongest argument in this
document: when a relation is wrong, fixing one symptom leaves the others standing. Ask what the two
sides of your change are, and assert their agreement rather than each side.

**Not one is a wrong line in isolation.** Each side is individually correct, so a unit test of
either passes. When two values must agree, assert the agreement:

```cpp
EXPECT_LE( fadeEnd, maxViewDistance );                    // a range inside another range
EXPECT_LT( coarseMultiplier, emptySamplesBeforeCoarse );  // or the march stalls in place
EXPECT_NEAR( mapValue, coneValue, tol );                  // two implementations of one quantity
EXPECT_EQ( componentDefaults, presetRow );                // a mirror that must not drift
```

Two properties worth reaching for, because they catch whole classes rather than instances:

- **A bound.** "A step cannot scatter more light toward the eye than the source it integrates" —
  one assertion, and it catches any spurious factor of anything.
- **A monotonicity.** "More cloud lies above a low sample than a high one" — catches inversions
  and off-by-one indexing that a spot value never will.

## 5. Finding the mechanism: instrument, knock out, then change something

Every defect in the table above was found the same way, and none was found by reading code and
guessing. The method, in order:

1. **Render the suspect quantity as colour.** Write the sample count, the tier a ray ended in, the
   profile value, the fade weight — whatever your hypothesis is about — straight into the frame. A
   frame that is entirely red because every ray exhausted its budget is an argument no code review
   produces.
2. **Knock out one contributor at a time** and measure the artefact each time. Publish the table.
   The cirrus weave was pinned by measuring its screen frequency (it matched the jitter noise's own
   aliased fundamental exactly) and by rendering at Full instead of Half resolution, which HALVED
   its period — proving it was locked to the buffer grid, not the world.
3. **State the mechanism with numbers before you change anything.** "The stride is 700 m where the
   volume's structure is 391 m, 1.8x past its own Nyquist limit" is a finding. "The far field looks
   noisy" is not.
4. **Build the ground truth when you can.** A converged reference march (tiny steps, huge budget) is
   slow and worth it: it turns "better" into "17% below the reference instead of 25% above it".

**A hypothesis you disprove is a result — write it down.** Several agents saved the next one real
time by recording what was NOT the cause: the per-cell height bands were not the fringe, restricting
the clamp box to fresh checkerboard taps makes Ultra strictly worse, and shrinking the weather cell
makes the "clouds too low" complaint worse rather than better.

## 5a. Measuring performance on this machine

The frame-count slope is the house method: time `--shot-frames 300` and `--shot-frames 900`, then
`slope = (t900 - t300) / 600`, which cancels the ~20 s fixed startup.

- **The machine is shared with other agents.** Absolute slopes taken twenty minutes apart are not
  comparable — one measured run drifted 40% with no code change, and another produced a physically
  impossible result (more frames finishing faster). **Interleave A and B in one session and take the
  minimum of N**, never the mean: the mean measures your neighbours.
- **Say the machine was shared** in the report, and give the spread. A number without its noise
  floor is not a measurement.
- Prefer swapping a shader file over rebuilding when comparing — the shaders are cooked at runtime,
  so A/B needs no second binary.

## 5b. A refusal is a deliverable

If the measurement says a feature does not earn its cost, **say so with the numbers and do not build
it**. This is not failure and it is not laziness; four separate agents did it correctly, and one of
them built a 3D mip generator, measured it at six tenths of a grey level, and removed it again. What
is forbidden is shipping something that never fires, or shipping a knob that hides a defect instead
of fixing it. Record the refusal where the next person will look — a commit message, the requirement
doc, or a comment at the site.

## 6. Before you commit

1. `make Desert config=debug -j8` and `make Editor config=debug -j8` clean. New files ⇒
   `premake5 gmake` first (the generated makefiles list files explicitly).
2. Formatting with **llvm@18**, not the local toolchain:
   `git add` new files, then
   `/opt/homebrew/opt/llvm@18/bin/git-clang-format --binary /opt/homebrew/opt/llvm@18/bin/clang-format`.
   Local v22 disagrees with CI and passes work CI rejects.
2a. Static analysis with **llvm@18's clang-tidy**, on changed lines, against your branch's
   merge-base: `scripts/CI/CheckTidy.sh <merge-base>`. Read the EXIT CODE, not the output — 0 clean,
   1 findings, 2 the gate could not run. A macOS build runs it at the end by default.
3. The full sweep from §3 — zero failures.
4. Frames from §1 if anything on screen could have changed: **three elevations**, animation frozen
   for any pixel comparison, and the repeat-noise number beside your diff.
5. Report what you verified and *how*, and what you did not. A defect you name is a discussion; a
   defect found on review is a return.

**When several branches land in a row:** CI is configured `cancel-in-progress`, and Windows takes
~35 minutes against macOS's ten. Pushing a second commit while a run is in flight CANCELS Windows
every time, so a series of back-to-back merges validates on macOS only. Batch them into one push, or
wait for the run. **A cancelled job is not a pass and not a failure — it is no evidence.**

## Related

- `Docs/Clouds/DEV_CONTRACT.md` §2.3, §2.3.1, §2.4 — the same rules as the project's own contract,
  with the history of why each exists.
- `Docs/Clouds/RESEARCH_REFERENCE.md` §M — what rendering showed that reading the code did not.
- The `desert-engine-dev` skill — how the engine is built (architecture, conventions, footguns).
  This skill is about proving a change works; that one is about writing it.
- The `desert-engine-contract` skill — what may not ship at all (TODOs, stubs, dead settings,
  legacy paths), and the definition of done this verification is one clause of.

## 7. Two ways the verification itself has been wrong

Both were found by developers doing the verification honestly, not by anyone auditing it.

**The skip-list hid three real suites.** `DShaderParser`, `FontBaker` and `MeshSimplifier` build
binaries into `build/Bin/Tests/Debug` and were listed here as libraries. Every sweep the integrator
ran for a whole programme skipped them — 35 tests that never once executed. They passed when finally
run, so nothing was lost, but a failure in them would have been invisible for weeks.

**That direction is now closed by construction, and the check that used to guard it was unrunnable.**
It read `case "$b" in <your skip list>)` — a placeholder, so anybody who reached for it got a shell
syntax error rather than an answer, and a check nobody can run is worth exactly what an absent one is.
The list no longer names tools by hand (§3 derives that half from `ls Tools`), so a real suite cannot be
called a library any more: being a suite means having a directory under `Desert/Tests`, not under
`Tools`. What is left of the hazard lives in the other direction and the sweep now prints it —
`NO-BINARY`, one line per name the loop believed was a suite and could not run.

The one entry still typed by hand is the four engine projects and the two aggregates, and it is small
enough to audit by eye:

```bash
for b in $(ls build/Bin/Tests/Debug); do
  case "$b" in Desert|Common|Editor|Runtime|BuildAllTests|RunAllTests) echo "SKIPPED BUT IS A TEST: $b";; esac
done
```

**A moved or renamed source file leaves a stale `.d` behind, and `make` then demands the deleted
path.** After a task moved `SceneMigration.cpp` out of the engine, nine suites reported `BUILD-FAIL`
with *"No rule to make target `…/Engine/Core/Serialize/SceneMigration.cpp'"* — while the generated
makefile referenced only the new path. The old path was in
`build/Tests/Intermediates/…/<Suite>/SceneMigration.d`, which `make` includes, so the previous
build's dependency graph outlived the file. Deleting those intermediates fixed all nine.

CI never sees this — it checks out fresh — so it is a **local sweep reporting a failure that does
not exist**, which is as expensive as the reverse. It was the third stale artefact to lie to a sweep
in one day: a skip list that hid a real suite, `.make` files that are untracked and outlive a
rename, and this. **When a sweep fails right after files moved, delete the intermediates for the
failing targets before believing it** — and re-run `premake5 gmake`, because the makefiles are
generated and untracked too.

**Never trust the first render in a fresh worktree.** A task's baseline differed from its result by
110 pixels of 980 480, and four experiments eliminated every plausible cause — shader recompilation,
a changed asset header, an unrelated translation unit — before rebuilding the previous binary settled
it: the BASELINE was wrong, because it was the first ever run of the editor in that worktree. Discard
it and shoot again. The same signature had been attributed to shader recompilation by an earlier
task, and that attribution was wrong.

**And never trust the first render after a SHADER edit either — for a different reason.** §5a recommends
swapping a `.shader` over rebuilding, because the shaders are cooked at runtime. The cook cache is
content-addressed, so an edited shader misses it and is compiled during that load. Measured 2026-09-01:
one run immediately after editing a compute shader differed from its baseline on **100 % of pixels** and
never reproduced — five later runs of the same shader were byte-identical to the baseline. Discard the
first run after any shader edit, exactly as for a fresh worktree, and take the second. A knockout that
rests on one post-edit frame is not a knockout.

### A cross-tree pixel A/B is silently incomparable by default

**Found 2026-09-06, and it nearly invalidated a whole verification.** Comparing a frame taken in the
main tree against one taken in an agent worktree looks like the obvious A/B. It is not: the viewport
size comes from **`Editor/imgui.ini`, which is UNTRACKED**. The main tree shot at 1104x668, the
worktree at 1280x766, and the worktree's own layout was still settling — one tile came out 32x2.

Nothing in the PNG says which resolution produced it. A pixel diff between two such frames is
meaningless in both directions: "identical" cannot happen, and "different" says nothing about the code.

The developer caught it only because **the log prints the trace resolution**, not because any check
fired.

**Before any two-tree comparison:** copy one `imgui.ini` to both trees, freeze it for the whole run,
and restore the owner's afterwards. Or take both frames in ONE tree by swapping the binary — the
shaders cook at runtime, so a shader-only A/B needs no second checkout at all.

**And measure the floor in the same pairing you are about to compare.** Two runs of the SAME command
in the SAME tree first; only then is a difference between trees attributable to the change. On
clock-driven content — particles, anything on `steady_clock` — that floor is NOT zero: measured at
4.4% / 6.4% / 0.26% of pixels across three views on the same day.

### Regenerate the makefiles after ANY merge that added a file

The generated `*.make` files list sources EXPLICITLY, so a merge that brings in a new `.cpp` produces
a tree where that file is simply not compiled. It does not fail at compile time — it fails at LINK
time, in another target, with an undefined symbol that names a function you can see in the source
right in front of you.

Caught 2026-09-06 on the lead, integrating И2: `MigratorMain.cpp` arrived with the merge, the
makefiles were not regenerated, and `SceneMigrator` failed to link on `RunSceneMigrator` — a symbol
whose definition was sitting in the working tree. Ninety seconds of confusion for a one-line fix.

```bash
CI=true premake5 gmake     # after every merge; free when nothing changed
```

The `CI=true` matters: without it the test projects are not generated at all, and the sweep silently
covers less than it claims to (§3).

And distinguish the two failures before diagnosing either. `Terminated: 15` is SIGTERM — something
killed the build, usually a harness timeout under load — and says NOTHING about the code. An
`Undefined symbols` link error after a merge is almost always this missing regeneration, not a defect
in what was merged.

## 8. Who runs what, when several agents share one machine

**Added 2026-09-06, from a measured jam.** Eight agents each running the full sweep in their own
worktree is nine runs of the same 105 suites — theirs plus the lead's after the merge. Measured cost:
72–80 clang processes on 8 performance cores, load 135, and **nothing finishing** for the better part
of an hour. The suites were not the problem; running them nine times was.

**The developer proves their own change. The lead proves the integration.**

A developer runs, in their worktree:

1. the three targets by exit code — `Desert`, `Editor`, `Runtime` (compilation is not delegable);
2. **their own new suites**, and any suite whose subject they touched;
3. the three that fire from changes nowhere near their name — `ComponentReflection`,
   `SettingConsumers`, `ShaderCacheKey` (see §3);
4. frames, if the change can alter what appears on screen. Never delegable, never skippable.

The **full sweep runs once, by the lead, in the main tree after the merge** — where it is also the
only place it can catch what the merge itself created. A distant suite broken by a developer's change
is found there, one merge later, and merges are serialised anyway.

**Wrap every build in the queue**, or the machine goes back to starving itself:

```bash
scripts/MacOS/BuildSlot.sh make Editor config=debug -j4 CC="ccache clang" CXX="ccache clang++"
```

It passes the exit code through unchanged, so the `echo $? > my_rc.txt` discipline still holds.

**What this does NOT relax.** A developer who skips their own new suite, or ships a render change
without a frame, has not done the work — the lead's sweep is a net for integration, not a substitute
for proving your own change. And a developer whose change plausibly reaches far (a shared header, a
format, a build file) still runs the full sweep and says so.
