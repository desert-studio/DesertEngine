# DesertEngine

C++20 / Vulkan game engine: EnTT ECS, RDG renderer, ImGui editor, Lua, its own shader language, JobSystem. Windows and macOS. Answer the owner in Russian; code, commits and comments in English.

## Layout
- `Desert/` — modules (`Common`, `CoreReflection`, `Schema`, `PhysicsCore`, `Audio`, `Net`, `Water`, `Landscape`, `Hair`, `Destruction`, `VFX`, `UI`, `TextCore`, `Render2DCore`) and the engine `Desert/Desert`. Layers: Common → modules → engine → `Editor/`, `Runtime/`, `Server/`.
- `Editor/Resources/Shaders/` — `.shader` programs, `.glslh`. `Projects/` — content. `Scenarios/` — `.scn` acceptance scripts. `scripts/Dev/` — land, build, census. `Tools/` — header tool, DesertCtl, migrators.

## Work
- A feature goes whole and once: skill `desert-engine-feature` (design by `architect` → one `engineer` run → `reviewer` → batch build → `acceptance` → land the same day).
- Architecture first: skill `desert-engine-architecture`; `python scripts/Dev/arch_census.py` may only fall.
- Visible things: skill `desert-engine-look`. Shipping rules and definition of done: `desert-engine-contract`. Code conventions and performance: `desert-engine-dev`. Proof: `desert-engine-verify`. Long polish: `desert-engine-gauntlet`.
- The lead's procedure is `.claude/skills/desert-engine-contract/LEAD_PROTOCOL.md`; read the section you need, never the whole file.

## Build and run
- Windows: `bash .claude/tools/build_land.sh F:/DesertEngine <log>`, in the background, one batch, never while the editor runs; below-normal priority, `-j6`. Agents never build.
- Landing: `scripts/Dev/land_dev.sh`; trunk is `dev`, branches live a day.
- Editor: only through a scenario, one at a time. Writing and running tests is off until the owner re-enables it; test projects still compile.

## Hard rules
- No TODO, stub, silent fallback, legacy path, new singleton or content name in a mechanism.
- One source of truth per value; centimetres; migrate serialized data in the files, at once.
- No exceptions in tasks or destructors; errors are values.
- Never commit frames, generated projects (`*.vcxproj*`), `Editor/Resources/Assets/`, `ThirdParty/*` extras, autosaves.
- Never kill a process by image name. Never `sed -i` on a CRLF file.
- Notion only through `Agent(subagent_type: "board")`. Tasks and contracts live there, not in `Docs/`.
