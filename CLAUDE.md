# DesertEngine

C++20 / Vulkan game engine: EnTT ECS, RDG renderer, ImGui editor, Lua, its own shader language, JobSystem. Windows and macOS. Answer the owner in Russian; code, commits and comments in English.

## Layout
- `Desert/` — modules (`Common`, `CoreReflection`, `Schema`, `PhysicsCore`, `Audio`, `Net`, `Water`, `Landscape`, `Hair`, `Destruction`, `VFX`, `UI`, `TextCore`, `Render2DCore`) and the engine `Desert/Desert`. Layers: Common → modules → engine → `Editor/`, `Runtime/`, `Server/`.
- `Editor/Resources/Shaders/` — `.shader` programs, `.glslh`. `Projects/` — content. `Scenarios/` — `.scn` acceptance scripts. `scripts/Dev/` — land, build, census. `Tools/` — header tool, DesertCtl, migrators.

## Where each rule lives (one place each)
- Numbers and paths of the process (agents, calls, brief and report sizes, builds, board batches): `.claude/tools/limits.py` — documents name the constant, never the value.
- A feature, whole and in one day: skill `desert-engine-feature`; its state: `python .claude/tools/studio.py digest`.
- Architecture (code reference = C++ patterns): skill `desert-engine-architecture`; `python scripts/Dev/arch_census.py` may only fall.
- Visible things (reference = UE / AAA look): skill `desert-engine-look`. Definition of done: `desert-engine-contract`. Code conventions and performance: `desert-engine-dev`. Proof: `desert-engine-verify`. Long polish: `desert-engine-gauntlet`.
- The lead's procedure: `.claude/LEAD_PROTOCOL.md`, by topic; dated traps: `.claude/LESSONS.md`. Read the section you need.
- The wrapper checks itself (budgets, stale facts, one rule in one file): `python .claude/tools/wrapper_lint.py`.
- What the hooks refuse: `.claude/tools/agent_guard.py` (its self-check line at session start is the list).

## Build and run
- One build per batch, by the lead, through `.claude/tools/build_land.sh` (build_gate refuses every other entry); never while the editor runs. Agents never build.
- Landing: `scripts/Dev/land_dev.sh`; trunk is `dev`, a branch lives a day.
- Editor: only through a scenario (`.claude/tools/play_scenario.py`), one at a time. Writing and running tests is off until the owner re-enables it; test projects still compile.

## Hard rules
- No TODO, stub, silent fallback, legacy path, new singleton or content name in a mechanism.
- One source of truth per value; centimetres; migrate serialized data in the files, at once.
- No exceptions in tasks or destructors; errors are values.
- Never commit frames, generated projects (`*.vcxproj*`), `Editor/Resources/Assets/`, `ThirdParty/*` extras, autosaves.
- Never kill a process by image name. Never `sed -i` on a CRLF file.
- Notion only through `Agent(subagent_type: "board")`. Tasks and contracts live there, not in `Docs/`.
