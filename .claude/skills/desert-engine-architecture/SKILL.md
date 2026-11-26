---
name: desert-engine-architecture
description: The architecture rules of DesertEngine with the checks that enforce them — ownership, layers, subsystem lifetime, data flow, module boundaries, what counts as an anti-pattern. Use when designing or reviewing any change that adds a class, a system, a pass, a setting, a global, an event, or touches a file over 2500 lines.
---
# Architecture

Machine check: `python scripts/Dev/arch_census.py` (a ratchet: the numbers in `arch_baseline.json` may only fall; `--list RULE` shows the places). `land_dev.sh` and `build_land.sh` refuse a rise. A rise needs `--update --accept-rise "reason"`, by the lead, and the reason stays in the file. After a fall: `--update`.

## Layers
`Common` → modules (`CoreReflection`, `Schema`, `PhysicsCore`, `Audio`, `Net`, `Water`, …) → `Desert` (engine, renderer) → `Editor` / `Runtime` / `Server`. A lower layer never includes or names a higher one. The renderer reads a frozen frame packet, never the scene. Editor code is never reachable from a packaged game.

## Ownership
1. Every value has ONE owner; everybody else asks it. Two copies that must agree are a defect even when they agree today.
2. Lifetime is a tier — Engine, Editor, Project, World, LocalPlayer — and a subsystem is registered in its tier and passed as a dependency. No new `GetInstance`, no mutable static, no global current-scene.
3. World data is per world: two worlds in one process (PIE server and client) must not see each other.
4. A mechanism never names content (asset name, scene name, project name, sound category, team). Content is data: an asset, a project setting, a registered type.
5. A list of systems, passes or types exists once. A new one registers itself; it is not added to three lists.

## Flow
- Change is an event or a version number, not a poll per frame; derived data is rebuilt when its input's key changes and at no other time.
- Failure is a value with a reason that reaches the user. No silent fallback, no default that hides a refusal. No exceptions in tasks or destructors.
- A renderer pass declares what it reads and writes to the RDG; a hidden global resource is a defect. Where a quality tier drops work, it is a scalability group, not an `if`.
- New serialized field = new version + migration of the files, in the same branch.

## Size
A file over 2500 lines may not grow by 100. Split by responsibility (a pass, a system, a panel), never by line number. Known debt: `SceneRenderer.cpp`, `MeshRenderer.hpp`, `MeshRendererForward.cpp`.

## Open debt (COUPLING-AUDIT, checked 2026-11-25)
Net rates as constants (`Net/Replication.hpp`) instead of project settings; systems list duplicated (`RuntimeLayer.cpp`, `SceneWorkspace.cpp`); `LevelTravel` and `Input`/`EngineContext` singletons; `SoundClass` enum instead of an asset; `Window.hpp` title. A change that touches one of these fixes it whole.

## Questions a design answers
Who owns it? Who may write it? When is it rebuilt? What does a second world / view / player do? What is deleted? What does UE do, and why is ours the same or different? What does it cost in ms and MB, and at which tier?
