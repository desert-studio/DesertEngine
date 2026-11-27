---
name: desert-engine-contract
description: The delivery contract of DesertEngine — what cannot ship, the definition of done, the no-legacy migration rule, the measured refusal, and which of the three settings files (editor.json, .deproj, .desce) owns a new field. Use before handing in any task, when reviewing a hand-in, when adding a serialized field or a setting, or when deciding whether a task is finished.
---
# The delivery contract

This file owns four things: what cannot ship, what "done" means, how old data and old paths die, and where a setting is stored. Everything else has its own owner and is not repeated here:

| Rule | Owner |
|---|---|
| layers, ownership, one source of truth, patterns | skill `desert-engine-architecture` |
| how a feature runs (design, packages, file ownership, review, build, landing) | skill `desert-engine-feature` |
| engine conventions and performance | skill `desert-engine-dev` |
| scenarios, frames, measurements | skill `desert-engine-verify` |
| calls, read ranges, brief and report sizes, waiting | `.claude/tools/limits.py`; the hook prints them at the start of every run |
| who builds, who lands | `CLAUDE.md`, «Build and run» |

**Unfinished code does not exist in a branch.** If a task needs a dependency, write the dependency and then the task. A task ships whole or it does not ship; the size is counted honestly once, in the design.

## Cannot ship (any one is a return)
1. `TODO`, `FIXME`, `XXX`, `HACK` in new code. What is out of scope is a line of the report, not a comment in the source.
2. A stub: a function returning a constant "for now", an empty body "so it links", a parameter accepted and ignored.
3. A dead setting. Every exposed parameter is wired end to end: component → serialization → editor UI → GPU → a visible effect.
4. A silent fallback. A resource that did not load, a size that did not match, a shader that did not compile is logged with names and numbers and returned as an error; a default is never substituted quietly.
5. An empty successful answer. "Zero results" and "not ready yet" must be distinguishable by the caller. The shape to look for: a container filled from the same source as the question asked of it (a census built from the keys the source still offers can never contain the removed one).
6. A new third-party dependency without the owner's written yes.
7. A second path beside the old one (see «No legacy»).
8. A push to `dev`. An agent commits and pushes its own task branch; integration is the lead's.

## Required
- **Read the file, not the line.** Opening a file for one constant, also ask what the file's design is and whether it is right, and name what you find in the report even when it is outside the task. Architectural defects sit in files that work; a finding costs one line, not finding it costs the later rewrite.
- **Text is formatted, never glued.** `std::format` / `std::format_to` into one buffer with a literal format string; a shape used twice is one named function or one `constexpr std::string_view`; text a user reads goes through Localization with named arguments; JSON and manifests go through their writer (`Common/Json/Json.hpp`); paths are `std::filesystem::path` joined with `/`. A glued line you touch is migrated in the same change.
- **Per-frame renderer state.** A new uniform or storage buffer, material or descriptor set stores its state per (frame × renderer slot). Breaking this looks like "the preview shows the viewport's camera".
- **Errors are values**: `Common::BoolResultStr` / `Common::ResultStr<T>` with `NO_DISCARD`, as in the neighbouring code; not a bare `bool`, not an `assert` standing in for handling.
- **Comments say why, in English**: the symptom this solves and what the naive version would do. Naming and formatting follow the neighbouring code; clang-format is applied by the hook on every edit.

## Definition of done
1. The package is whole: every difference the design assigns to it (`Закрывает:`) is closed, with `file:line` in the report.
2. Nothing from «Cannot ship»; no new compiler warning expected (re-read every changed file once as the compiler would: types, includes, both sides of every shader binding).
3. `python scripts/Dev/arch_census.py` reports no rise.
4. Serialized data the package owns is migrated in the files, in the same branch.
5. The scenario of the feature exists and states every «Тогда [кадр]» of the design; the batch build and the frames are the lead's and the `acceptance` agent's proof, not the author's claim.
6. The report says what was done, what was decided and why, what differs from the design and for what reason, and what only the build or a frame can prove. A divergence you name is a discussion; one found on review is a return.
7. The owner's audit, answered in writing for the questions the task deserves: Is this the right shape or the easiest one from here? Is it convenient for the person who uses it daily? What can be fixed cheaply while we are here? What is unfinished? What does it open up? "Landed with a named remainder" is not done: the remainder is closed, or it is filed and the parent stays open until it lands.

## Scenarios run through a queue, one runner per machine (owner 11-26, SCN-QUEUE)
As UE Gauntlet/Horde: a scenario run is a job in a queue, not a wait in the lead's head.
- The lead, or the agent handing in a branch, only adds a job: card code, tree, `.scn`. Waiting for the editor, or waking the lead because "the editor is free", is not allowed.
- Each machine has one runner (`acceptance`). It takes jobs in order without pauses, plays `play_scenario.py`, looks at the frames and writes the verdict to the card. There is still one editor per machine.
- Windows is the second runner with its own queue. Render scenarios with HW RT, and anything a Windows frame must prove, go there; the rest go to the first free machine.

## A measured refusal counts as done
If the work does not earn its cost, the deliverable is the measurement and the refusal. The rules that make it a refusal and not a shrug:
- Numbers, not impressions: what was measured, on what, and the noise floor.
- What it stands on: the nature of the task (does not age), our own content (we measured OUR input; such a refusal carries a return condition), or a premise that has since expired (the refusal is void).
- Recorded where it will be found: the commit message, the card, a comment at the site.
- A hypothesis you disproved gets the same treatment.
- A refusal that is reviewed has two outcomes only: it is justified again in writing, or a task is filed. On a return the input is changed first and measured second; re-measuring the old input repeats the old answer.

Still forbidden: shipping something that never fires, a knob that hides a defect, a budget constant raised until one camera angle looks acceptable.

## No legacy — migrate at once, in the files
1. The old path is deleted by the change that replaces it: a moved field is gone from the struct, the serialization, the UI and the shader. No "deprecated but still read", no flag, no `#ifdef`, no two branches in the renderer.
2. No double writes to the old and the new place.
3. Data migrates once: a file has a version, the migration raises N to N+1 explicitly, the runtime knows only the new model.
4. The migration function is pure: old property tree in, new one out; no GPU, files or global state; missing and malformed fields are handled and logged.
5. Scenes and assets in the repository (`Projects/*/Content/`) are converted by the same task through `scripts/Dev/migrate.sh` and committed.
6. Migration code names the two versions it connects in a comment. It destroys the old format; it does not support it.
7. A migration logs which file, from which version to which, and how many fields moved.

Rolling back is `git revert`, never a kept copy of the old subsystem.

## Where a setting goes: three files, one question order
- `~/.desertengine/editor.json` — one person's copy of the editor: what an installation remembers across projects, and what two people on one project may hold differently at the same moment. Never anything the shipped game needs: the packaged Runtime does not open this file.
- `<Name>.deproj` — what the product is, for everybody: identity, where content lives, which level boots, the format version. Nothing that varies per level or per machine, nothing derivable.
- `<Name>.desce` — what the world is: entities and the level-wide policy a designer authors (render path, shadows, grade, lens, wind, gravity). Not what a viewer draws on top, not what a machine can afford.

For a new field, in this order:
0. Does it describe the file itself (format version, unit generation)? Then it is that file's metadata.
1. Would two people working on the project at the same time legitimately want different values? → `editor.json`.
2. Does the value differ from level to level? → `.desce`.
3. Otherwise → `.deproj`.

Question 1 is first because it is about a conflict, and a conflict beats a scope: a quality knob answers "yes" to 2 and 3 as well, and any other order files it in a shared file. Quality knob or authored look? Set it to its cheapest value: the level is merely rendered worse → machine quality; the level is mis-authored (GI off is a darker room, not a coarser one) → level data.

- A fourth config file is the owner's decision, not the task's.
- The other files in `~/.desertengine` are not settings stores: `projects.json` and `engines.json` are cross-process registries, `Layouts/*.ini` and `imgui.ini` are ImGui's own state.
- The rule is enforced by `Desert/Tests/Engine/ConfigOwnership`, which lists each file's fields through the code that writes the file; a known violation is a row of its debt register naming the task that owns the move.
