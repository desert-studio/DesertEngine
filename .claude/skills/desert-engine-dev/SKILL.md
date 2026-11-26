---
name: desert-engine-dev
description: Engine conventions and performance rules of DesertEngine — errors as values, serialization and codegen, the ECS system model, the RenderCommandBuffer and Vulkan lifetime rules, reversed-Z, the shader language and its cache, the JobSystem, the optimisation workflow and the engine-specific footguns. Use when writing or reviewing engine, editor or shader code.
---
# DesertEngine — conventions and performance

How the code is structured (layers, ownership, patterns, one source of truth) is the skill `desert-engine-architecture`; what may ship is `desert-engine-contract`. This file is the engine's own facts: what already exists and must be used instead of being reinvented. Where a subsystem lives: `.claude/CODEMAP.md`; a name: `scripts/Dev/sym.sh <Name>`.

## Proportion
A one-line fix gets a one-line change. A new subsystem, a new serialized format or a new render pass gets a design first (skill `desert-engine-feature`). Never implement a structural change straight away; never wrap a small fix in process.

## Core conventions
- **Errors**: `Common::ResultStr<T>` / `Common::BoolResultStr` for anything that can fail on data (bad shader, missing parameter, parse error): `Common::MakeError<T>("…")`, `MakeSuccess(value)`, caller checks `IsSuccess()`. See `Common/Core/ResultStr.hpp`.
- **Serialization**: `rfl::json`; a serializable struct is a plain aggregate. A new field is a new version plus a migration of the existing files in the same branch.
- **Reflection and codegen**: `Tools/DesertHeaderTool` generates into `Engine/Generated/`. Never hand-edit a generated file; change the annotation and regenerate.
- **No generated-language walls in C++.** Shader boilerplate lives in shared `.glslh` includes configured by `#define`s (`Common/GraphVertex.glslh`, `TimeUB.glslh`); a generator emits structure and the unique expressions only.
- **Units**: the world unit is the centimetre. Touching an old subsystem, sweep it for lengths written in metres (a draw distance of 45 was 45 cm); a dimensionless ratio beside them says so in a comment.
- **Memory** in a hot path, in this order: stack → arena → pool → frame allocator → heap. Scratch buffers are reused across frames.

## ECS
- A system derives from `Desert::ECS::System` and implements `Update(entt::registry&, Graphic::Render::RenderCommandBuffer&, const Common::Timestep&)`.
- `CanRunParallel()` returns `true` only for a read-only system: no structural registry change, no write another same-frame system reads. Consecutive parallel-capable systems run concurrently on the JobSystem, each with its own command buffer; buffers execute in registration order, so draw order stays deterministic. Anything touching Lua, input, physics or components other systems consume stays `false`: a wrong `true` is a data race.
- Iterate data (`registry.view<…>()`), not per-entity virtual calls.

## Rendering
- Draw work is recorded into a `RenderCommandBuffer`, never issued immediately.
- Editor-only visuals (grid, collider debug draw) are injected through the Editor Pass API as real 3D passes, not special cases in the core renderer.
- Data-driven materials (`MaterialComponent.ShaderName`) receive engine data through opt-in uniform blocks found by reflection name (`TimeUB`, `DirectionLightsUB`). New engine data for shaders is a new named block filled in `MeshRenderer`.
- Post, IBL and exposure are compute passes; prefer compute and shared LUTs to per-fragment recomputation.
- **Vulkan lifetimes**: pipeline layouts, push-constant ranges and descriptor set layouts outlive the pipelines that reference them and are owned by a stable container, never a temporary (a dangling `VkPushConstantRange` cost FPS, commit `868b6b2`).
- **Depth is reversed-Z on `DEPTH32F`**: every projection comes from `Core/Projection.hpp`, depth clears to 0, the default compare is `Greater`. `ZTest Less` in the shader language still means "the nearer fragment wins"; that mapping lives once in `PipelineCache` — never mirror it by editing a `.shader`. Shadow cascades keep standard Z on purpose (orthographic) and spell it with a raw compare op.
- **Specialization constants** need no permutation machinery: `ComputePipeline`'s spec takes an int32 vector. Vulkan silently ignores a constant id the module does not declare, so the id is checked in the SPIR-V, or the result is a correct, slow frame with no error.
- `texture()` in a compute shader is undefined (no derivatives): always `textureLod(…, 0.0f)`.

## Shaders
- The authoring format is the Desert Shader Language (`.shader`): `Shader "Name" { Domain … Properties … State … Vertex{} Fragment{} Pass "Depth"{} }`. Only `Surface` and `Terrain` domains are assignable to a material (`ShaderProgramMeta::IsUserAssignable`).
- SPIR-V is cached by a hash of the resolved source in `Cooked/ShaderCache`. Whatever a shader depends on (includes, defines) must take part in the hash, or stale SPIR-V is served. Hash the inputs, skip unchanged work: the model for every expensive repeatable cook.
- The Shader Graph compiles a node `Document` to `.shader` source; one node catalogue (`Specs()`) drives both the palette and the compiler.

## Concurrency
- `Common::JobSystem` is the one place for CPU-parallel work; new code receives it as a dependency. `Submit` (fire and forget), `Async` (future), `ParallelFor` (blocks; the calling thread works too).
- A task body is `noexcept` and returns its error as a value; `Join` never throws.
- No ad-hoc `std::thread` / `std::async`.
- A job does not touch the GPU, the AssetManager or the registry: gather on a worker, mutate shared engine state on the owning thread.

## Optimisation
1. Measure first: Optick scopes (`OPTICK_EVENT`) and the in-viewport Perf HUD (top scopes). No profile, no performance change.
2. Cut per-frame allocation: reuse command buffers and scratch containers; `reserve`d or pooled storage in hot loops.
3. Cache repeatable work by content hash.
4. Parallelise CPU-bound, independent, read-only work through the JobSystem; draw order must stay deterministic.
5. Batch GPU work; no per-draw pipeline or layout churn, no redundant descriptor updates.
6. Re-measure in the configuration `scripts/Dev/dev_config.txt` names, by the method of skill `desert-engine-verify` («Performance»).

## Footguns of this engine
- A mechanism that names content (a scene, an asset, a level) — the rule is in `desert-engine-architecture`; in review this is the first thing to look for.
- A mirror with one reader. A C++/GLSL pair asserted equal exists because two implementations of one quantity drift; when only one side reads the value, the second copy is the defect.
- A default that hides a missing serialized field: that is a silent fallback, not compatibility.
- Two numbers that must agree and nothing says so. State the relation in code: derive the default from the relation, let a preset choose inside a stated range, and log a warning naming the number the scene should have had (`CloudWeatherScale`, `CloudMarchScale`, `CloudLayerAspect` are the pattern). A warning about physics is worth shipping; a warning about taste is not.

## When you finish
The report follows `desert-engine-contract` («Definition of done»). For a performance change it carries the before and after numbers with their noise floor.
