---
name: desert-engine-contract
description: >
  The engineering contract this project is reviewed against — what may not ship (TODOs, stubs,
  dead settings, silent fallbacks, legacy paths), what every change must do (interface first,
  layers unmixed, one source of truth per value, centimetre units, per-frame renderer state),
  how migrations are done (immediately, in the files, tested), and how work is delivered and
  reviewed. Use this when starting or finishing a task, deciding whether something is ready to
  hand over, adding a parameter or a component field, replacing an existing subsystem, changing a
  serialized format, or working alongside other people on the same repo.
---

# The delivery contract

**This applies to the whole repository, not to one programme.** The rules were first written down
for the sky-and-clouds work, which is where the defects that motivate them were paid for, but every
one of them is about how code is delivered here and none of them is about clouds.

The authority is the file bundled next to this one, **`DEV_CONTRACT.md`** (Russian) — read it when a
rule's wording or its history matters, because it carries the specific defect behind each rule. This
SKILL.md is its operative core.

**The contract used to live at `Docs/Clouds/DEV_CONTRACT.md` and no longer does.** `Docs/` was taken
out of version control on 2026-09-04 — it is a working directory for one person on one machine and
is not shared. The bundled copy here is now the only tracked one, and it was promoted from the
Docs copy at that moment because the bundle had gone stale: 260 lines against 343, missing among
other things the MoltenVK environment variables without which the editor dies before its first
frame. **If you edit the contract, edit it here.** Any surviving citation of `Docs/Clouds/DEV_CONTRACT.md`
in a source comment points at a path a fresh clone does not have.

**How to use it:** the rules in §1 are absolute; a change that breaks one is returned regardless of
how good the rest is. Everything else is judgement, and the contract says which way to lean.

---

## 0. The rule the rest follow from

**Unfinished code does not exist in a branch.** If a task needs a dependency to exist first, write
the dependency and then the task. Not "a stub for now", not "we'll finish it later", not "left a
TODO for whoever comes next". A task ships whole or it does not ship.

That is expensive exactly once — at planning, when the size is counted honestly. After that it
saves weeks, because nobody builds on sand.

---

## 1. Cannot ship (any one of these is an automatic return)

1. **`TODO`, `FIXME`, `XXX`, `HACK` in new code.** None. If something is not done, it is not
   delivered. If something is deliberately out of scope, that is a line in the report, not a
   comment in the source.
2. **Stubs.** A function returning a constant "for now"; an empty body "so it links"; a parameter
   accepted and ignored.
3. **Dead settings.** Every parameter exposed to the outside must be wired end to end: component →
   serialization → editor UI → GPU → a visible effect. A slider that moves nothing is a TODO
   wearing a feature's clothes, and it is how systems end up where "half the settings don't work".
4. **Silent fallbacks.** A resource that did not load, a size that did not match, a shader that did
   not compile — log it with the reason and the actual numbers (`LOG_ERROR` with names and values).
   Never substitute a default quietly. Debugging blind is the most expensive thing in graphics.
   **An EMPTY SUCCESSFUL answer is a silent wrong answer, not a refusal.** Zero results and "not
   ready yet" must be distinguishable by the caller, or the caller will read the first as the second
   and be wrong with no way to tell. Two unrelated instances landed on one day: the control channel
   answered `commands` with an empty `Open` group for the first ~90 seconds while the preloader ran,
   and a client cannot tell "this project has no assets" from "ask again later"; and a content
   census built its on-disk set from the keys the source was still offering, so the one file a
   removal is about was the one it could never contain — every deletion became a no-op and **the
   feature reported itself as working**. No test caught the second; a run of the real editor did.
   The shape to watch for: a container whose contents are derived from the same source as the
   question being asked of it.
5. **New third-party dependencies without written agreement.** OpenVDB at runtime in particular: no.
6. **Editing files another task owns.** Each task owns a file list. Need someone else's file — ask;
   the answer is either to widen the ownership or to sequence the tasks. A conflict in a shared
   header costs more than a minute of coordination.
7. **`git push` and anything touching `dev`** when working as one of several developers —
   integration is the lead's. Commits **inside your own worktree** are how work is handed over and
   are expected. *(This one is conditional: when working solo with the user, they routinely ask for
   commit-and-push directly. Follow what the user asks for now over the multi-developer default.)*
8. **Keeping legacy alive.** See §4 — the old path is deleted by the same change that replaces it.

---

## 2. Required

### Read the file, not the line

**Standing rule, added 2026-09-05 at the owner's insistence and after I earned it.** When you open a
file for any reason — one constant, one call site, one grep hit — spend the extra minute on the
question the visit did not ask: *what is this file's design, and is it right?* Then **say what you
found**, even when it is outside your task. You are not required to fix it; you are required to name
it.

Why this is a rule and not a nicety: this project runs a defect-driven loop, where each task is born
from what the last one found. That finds a great deal, and it is structurally blind to anything
nobody has stumbled over yet. Architectural problems do not announce themselves — they sit in files
that work.

The failure this rule exists to stop, in the exact form it happened: I opened `AssetPreloader.cpp` to
check one extension constant, looked at that one line, and reported the file fine. It walks the whole
asset tree once *per asset type* (six passes over the mesh directory alone), `PreloadMeshes()` also
loads textures, animations, skeletons and materials, `ReloadCooked()` calls `PreloadMeshes()` meaning
"reload everything", a load-bearing ordering dependency is expressed only as statement order, and the
extension→type mapping is twelve hand-written pairs with nothing asserting they agree. None of that
was hidden. I read with a query instead of with judgement — the same "tool answering a different
question" this document warns about elsewhere, applied to reading. The owner had to ask why they were
finding these instead of me.

A finding costs one paragraph in the report. Not finding it costs whatever it costs later.

### Architecture

- **Interface before implementation.** The header first: types, signatures, resource ownership,
  invariants, with comments. Implementation after. If a task changes an agreed public interface,
  say so before changing it.
- **Layers do not mix.** `Engine/Core` knows nothing of Vulkan. `Engine/Graphic` knows nothing of
  ImGui or the editor. The editor does not reach into the renderer's internals. Data (an ECS
  component, serializable and POD-like) is separate from behaviour (a system or renderer).
- **One source of truth per value.** A parameter that lives in a component is not duplicated into
  `SceneSettings` "just in case". Duplicated state is a future desync bug.
- **RAII for resources.** Anything created on the GPU is released deterministically, waiting for
  device idle where that is required. Copy how the existing systems do it; do not invent a third way.
- **1 world unit = 1 centimetre.** Layer altitudes, radii, distances — all centimetres. Numbers from
  papers are almost always metres or kilometres: convert explicitly through `Common::Units::` and
  write the original figure in the comment. Half the mystery bugs in this engine were metre-era
  leftovers.

### Conformance to the engine

- **Per-frame renderer state is mandatory.** Any new uniform/storage buffer, material or descriptor
  set stores its state per (frame × renderer slot) — see `Docs/RENDERER_FRAME_STATE.md`. Breaking it
  looks like "the preview is peeking at the viewport camera" and takes days to find.
- **Errors through `Common::BoolResultStr` / `NO_DISCARD`**, as in the neighbouring code. Not
  exceptions, not a bare `bool`, not an `assert` standing in for handling.
- **Comments say WHY, in English.** What symptom does this solve, and what would happen if it were
  done naively. Not a restatement of the code.
- **Naming and formatting follow the neighbouring code.** No new style per subsystem.

### Verification

Its own skill: **`desert-engine-verify`**. In one line — if the change alters what appears on
screen, render a frame and look at it; "builds and tests pass" is not verification, and the four
most expensive defects in this project all shipped built, tested and unseen.

---

## 3. Definition of done

1. `make Desert config=debug -j8` and `make Editor config=debug -j8` build clean. New files ⇒
   `premake5 gmake` first (the generated makefiles list files explicitly, so new code silently
   misses the build otherwise).
2. No new compiler warnings.
3. Formatting clean on changed lines under **llvm@18**:
   `/opt/homebrew/opt/llvm@18/bin/git-clang-format --binary /opt/homebrew/opt/llvm@18/bin/clang-format`
   (`git add` new files first — untracked files are skipped locally and checked by CI). Local v22
   disagrees with CI. As a developer, check against **your branch's merge-base**, not `dev`;
   checking against `dev` reformats other people's landed lines into your diff.
3a. **Static analysis clean on changed lines under llvm@18**, the same keg as the formatter:
   `scripts/CI/CheckTidy.sh <merge-base>` — 0 clean, 1 findings, **2 the gate could not run**, and 2
   is never a pass. A macOS build runs it for you at the end unless you pass `--no-analyze`; CI runs
   it as its own job. Changed lines and not the tree, for the same reason the formatter works that
   way: `.clang-tidy` reports five figures of diagnostics over the workspace as it stands.
4. No new TODOs, stubs or dead parameters.
5. Tests on the pure logic, written and passing — **all suites, not the matching one**, and frames
   if the render changed. See `desert-engine-verify`.
6. **A report.** What was done; what was decided and why; **what differs from the requirements and
   for what reason**; what was left out of scope deliberately. A divergence you name is a
   discussion. A divergence found on review is a return.
7. **The owner's audit, run on yourself before you call it done** (his standing rule, 2026-09-07).
   A task is finished when **everything the owner said about it is closed**, or the work **captures
   the essence** of what he asked — *and* when your own audit leaves no open question. The questions
   are not a ritual; pick the ones the task deserves and **answer them in writing**:
   - **Did we build it right?** Not "does it work" — is this the right shape, or the shape that was
     easiest from here?
   - **What is still worth improving?**
   - **Is it convenient?** Would the person who uses this every day thank us or work around us?
   - **What can be fixed now, cheaply, while we are here?**
   - **What is left unfinished?** Name it. "Landed with a named remainder" is NOT done.
   - **What does this open up?** A seam that now makes three other things possible is part of the
     result and belongs in the report.

   **The trap this closes:** a task can pass 1–6 completely — builds, tests, frames, honest report —
   and still be unfinished, because the report *names* the remainder instead of closing it. Naming a
   gap is the minimum, not the finish line. If the remainder genuinely belongs to another task, it
   must be filed **and the parent stays open until that one lands**.

### A measured refusal counts as done

If the work turns out not to earn its cost, **the deliverable is the measurement and the refusal**,
not a feature nobody should have shipped. That is a completed task, and this project has accepted
several: an SDF step bound that could only ever fire in a sky with no clouds in it; a
quarter-resolution atlas that cost a third of the channel it was meant to preserve; a 3D mip
generator that was **built, measured at six tenths of a grey level, and removed again**; a weighted
metering mask whose own control series moved the frame by one 8-bit level.

The rules that make it a refusal rather than a shrug:

- **Numbers, not impressions.** Say what you measured, on what, and what the noise floor was.
- **Say what would change the answer.** ("This only pays once a stretch of sky can be declared
  voxel-only.") The next person needs to know when to revisit.
- **Record it where it will be found** — the commit message, the requirement entry, or a comment at
  the site. A refusal nobody can find gets re-derived.
- A hypothesis you **disproved** is worth the same treatment. Knowing that the height bands were not
  the horizon fringe, or that shrinking the weather cell makes "clouds look too low" worse, saved
  the next agent hours.

What is still forbidden: shipping something that never fires, a knob that hides a defect instead of
fixing it, or raising a budget constant until one camera angle looks acceptable.

---

## 4. No legacy — migrate immediately, and in the files

Compatibility "just in case" is how a project ends up with two paths forever, one of which nobody
tests.

1. **The old path is deleted by the change that replaces it.** A field moved to a new component is
   gone from the struct, the serialization, the UI and the shader. Not "deprecated but still read",
   not a "use the new system" flag, not an `#ifdef`, not two branches in the renderer.
2. **No double writes.** Never write a value to both the old and the new place "so nothing breaks".
3. **Data migrates once, at load, and is written back in the new form.** A scene has a version;
   the loader raises N to N+1 through an explicit migration and then works only with the new model.
   The runtime knows nothing about the old format.
4. **The migration function is pure and tested.** In: the old property tree. Out: the new one. No
   GPU, files or global state. The test covers missing and malformed fields.
5. **Scenes in the repository are converted by the same task.** Everything under
   `Resources/Assets/Scenes` is saved in the new format and committed. No file is left keeping the
   old path alive.
6. **Migration code has an expiry.** It raises a specific version to a specific version, and says so
   in a comment. It does not "support the old format" — it destroys it.
7. **Silent migration is forbidden.** Log which scene, from which version to which, and how many
   fields moved.

The same applies inside the code: when a new subsystem replaces an old one, the old implementation
is deleted, not kept "in case we roll back". Rolling back is called `git revert`.

---

## 5. Working in parallel

- **The shared backbone goes first, by one person** — component structures, serialization, editor
  registration. Nobody else starts until it exists, or three people edit one header at once.
- **After the backbone, work on non-overlapping files.** Shaders, the render pass, the editor UI.
- **Own your files, ask for anyone else's.**
- **Say which files you are fencing off**, and expect the same. When several people work at once the
  scope fence is what makes a merge mechanical instead of archaeological — and the one conflict this
  project actually hit was a scene file two people edited for unrelated reasons.
- **`git stash` is shared across worktrees on this machine.** One `stash pop` pulled another
  engineer's untracked files into an unrelated tree. Prefer explicit commits; if you must stash,
  apply by name.
- **The scratchpad is shared too, and it overwrites silently.** Two developers on one day each lost
  a run to it: one had `shot.sh` replaced mid-task by another worktree's copy pointing at a
  *different binary*, the other lost `sweep.sh` and `shot.sh` together, which killed a sweep. Nothing
  warns you — the script simply becomes somebody else's and keeps running. **Prefix every scratch
  file with your task's name**, and if a measurement disagrees with the one before it, check that the
  script that produced it is still yours before you go looking for a defect in the code.
- **The machine is shared too.** Rendering and timing while someone else renders produces numbers
  that measure them, not you — see `desert-engine-verify` §5a before quoting a slope.

---

## 6. Where a setting goes: three files, and the one question that decides

Written down by К1 after the same question surfaced four times in a week (К2, К3, У6, У8, Д31) in the
shape *one container holding things with different owners and different lifetimes*. Nothing anywhere
said which file owned what, and the cost is measured: `ShowColliders: true` travelled through git in 55
of the 73 scenes that stated it, and five image-quality fields still sit in the LEVEL file, so a weak
machine cannot turn the picture down without editing a file that goes to everybody.

**One sentence per file — what is in it and, above all, what is not:**

- **`~/.desertengine/editor.json` — ONE PERSON'S COPY OF THE EDITOR.** What a user's own installation
  must remember between sessions and across every project, and whose value two people on the same
  project may legitimately hold differently at the same moment. NOT anything a second person opening
  the project must see, and nothing the SHIPPED GAME needs — the packaged Runtime never opens this
  file, so a value put here is a value taken away from the player.
- **`<Name>.deproj` — WHAT THE PRODUCT IS, FOR EVERYBODY.** The few facts every process that opens
  this project must agree on before any level exists: identity, where content lives, which level
  boots, the format's own version. NOT anything that varies from level to level, NOT anything that
  varies from machine to machine, and no field whose value is derivable — `Common/Core/Constants.hpp`
  already refused fourteen folder-name fields on exactly that ground.
- **`<Name>.desce` — WHAT THE WORLD IS.** The level's entities and the level-wide policy a designer
  authors and expects to travel with the level: render path, shadows, the grade, the lens, wind,
  gravity, the splash. NOT what a VIEWER draws on top of the world (К2 took ten fields out), and NOT
  what a MACHINE can afford (К3 owes five).

**For a new field — three questions, IN THIS ORDER. The order is the rule.**

0. Does the field describe the FILE ITSELF — format version, unit generation, the name it is filed
   under? Then it is FILE METADATA, belongs to whichever file it describes, and 1–3 do not apply.
   Exactly four fields may claim this; a fifth is a conversation, not a row.
1. **Would two people working on this project AT THE SAME TIME legitimately want different values?**
   Yes → `editor.json`.
2. **Does the value differ from one level to the next?** Yes → `.desce`.
3. Otherwise → `.deproj`.

**Why question 1 comes first** is the only load-bearing part. Questions 2 and 3 are BOTH true of a
quality knob — anti-aliasing does differ between levels if authored that way, and it is a project-wide
default if set that way — so any order that asks them earlier finds it a home and stops. Question 1 is
the only one whose "yes" is about a CONFLICT rather than a scope, and a conflict beats a scope: a value
two people must be able to disagree about cannot live in a file they share, whatever else is true of it.

**Quality knob or authored look?** Set it to its cheapest value: has the level been MIS-AUTHORED, or
merely RENDERED WORSE? Rendered worse → machine quality (`MeshLOD` off is byte-identical geometry near
the camera). Mis-authored → level data that happens to cost something (Forward and Deferred disagree
about cloud shadow on the ground; GI Off is a darker room, not a coarser one).

**It is enforced, not merely written.** `Desert/Tests/Engine/ConfigOwnership` enumerates each file's
fields through the same mechanism that WRITES that file, so a field added tomorrow reddens it before
anyone remembers this document. Known violations sit in an explicit debt register and every row must
name the task that owns moving it — the suite checks that, because an exception with no task name is
unreadable in a month.

**A fourth config file is a conversation with the owner, not a decision.** K3 has no valid
"put it in `editor.json`" answer: all five quality fields are read by `SceneRenderer`, which the
packaged game also runs, and `editor.json` is an Editor-target concept the Runtime never opens.

**`editor.json` is THE per-user settings store, and the neighbours in `~/.desertengine` are not
alternatives to it** — this matters because "user state" already has five files and only one of them
is a place to put a setting:

| file | what it is | may a setting go here? |
|---|---|---|
| `editor.json` | the user's editor settings | **yes — this is the one** |
| `projects.json` | recent-projects registry, written by the engine AND the launcher | no: a cross-process registry, not settings |
| `engines.json` | engine-install registry, same two writers | no: same |
| `Layouts/*.ini`, `./imgui.ini` | opaque ImGui dock state, written and parsed by ImGui itself | no: we do not author its contents |
| `asset_favorites.txt` | Content Browser pinned folders — plain text, absolute paths, unchecked write | **no, and it should not exist**: user state that belongs in `editor.json`, filed as debt |

Note `imgui.ini` is resolved **relative to the working directory**, not to `~/.desertengine` — it is
why a cross-tree pixel A/B silently compares two different viewport resolutions (`desert-engine-verify`
§7).

---

## Related

- `DEV_CONTRACT.md`, bundled beside this file — the authority, with the history behind each rule.
- `desert-engine-verify` — how to prove a change works (§2.3, §2.3.1, §2.4 of the contract).
- `desert-engine-dev` — how the engine is built: architecture, conventions, footguns.
