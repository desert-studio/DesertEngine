---
name: desert-engine-dev
description: >
  Architecture and performance guide for developing DesertEngine — the C++20 / Vulkan
  game engine in this repo (EnTT ECS, ImGui editor, Lua scripting, custom Desert Shader
  Language, JobSystem). Use this when adding or refactoring engine subsystems, rendering
  passes, ECS systems, shader tooling, or when profiling/optimizing frame time, memory,
  or load time. Encodes the project's conventions so changes stay consistent with the
  existing design instead of introducing a new style.
---

# DesertEngine — architecture & optimization

C++20 / Vulkan engine (MoltenVK on macOS/Apple Silicon, native on Windows). ImGui editor,
Lua gameplay scripting, custom shader language (`.shader` → SPIR-V), EnTT ECS, `rfl` (reflect-cpp)
serialization, standalone Runtime player + Project Hub. Build: premake5 + make; CI builds
Debug+Release and runs the full test suite on every push.

Before touching a subsystem, read its neighbours — match the surrounding altitude, naming, and
error style. This skill tells you *how the engine is built* so you extend it, not fork its style.

## A R C H I T E C T U R E FIRST (owner, 2026-09-29)

Start every change from how the system is structured CORRECTLY (UE's pattern or better) and build that. No time budgets, reduced
scopes, guesses or bridges as the design; measurements only prove the design works. See desert-engine-contract §00.

## Engine Architect Manifest

The mindset for every non-trivial change. This is *how to think*; the sections below are *what the
engine is*.

### Design philosophy

Every solution must satisfy these priorities, in order:

1. **Correctness** — never sacrificed. Not for speed, not for elegance, not for a deadline.
2. **Simplicity** — the least machinery that solves the actual problem.
3. **Performance** — this is a real-time engine; in a proven hot path (per-frame, per-entity,
   per-draw) performance rises toward the top, but *never above correctness*.
4. **Extensibility** — the next feature should slot in, not require a rewrite.
5. **Maintainability** — the person reading this in a year (often you) must understand it fast.

When more than one viable design exists: compare at least **three** alternatives, state the
tradeoffs explicitly, and choose the one with the **lowest long-term maintenance cost** — not the
one that's fastest to type today.

### Proportionality (read this before applying the process below)

Scale ceremony to the change's **size × reversibility**:

- **Two-way door** (small, local, trivially revertable — a bugfix, a field, a rename): just do it
  cleanly. Don't write three alternatives for a typo.
- **One-way door** (public API, on-disk/serialized format, threading model, a new dependency, a
  cross-subsystem contract): run the **full** process below. These are expensive or impossible to
  undo — earn them.

When unsure which door you're facing, treat it as one-way.

### Architectural process — never implement immediately

For one-way-door work, walk the pipeline and show your reasoning before code:

```
Problem → Requirements → Architecture → Public API → Internal Design
        → Implementation → Tests → Profiling → Documentation
```

Design the **Public API first** — it's the hardest thing to change later. Internal design and
implementation serve the API, not the reverse.

### No magic dependencies

Never introduce a new third-party dependency unless explicitly requested. Before proposing one,
justify: **why** it's needed, what **alternatives** exist (including "write the small piece
ourselves"), and its **binary-size / compile-time / runtime** cost. A dependency is a permanent
liability in `ThirdParty/`, a one-way door — the bar is high, and "it's convenient" is not enough.

### Code Review Mode

Review every change as if it were an incoming PR to this engine. Hunt specifically for:

- **ownership & lifetime** — dangling refs, use-after-free; Vulkan handles (pipeline layouts,
  push-constant ranges, descriptor set layouts) outliving nothing (see the `868b6b2` FPS bug).
- **threading** — data races on the JobSystem, `CanRunParallel()` set `true` on a system that
  writes shared state; anything touching GPU/AssetManager/ECS from a worker.
- **performance smells** — cache misses / poor locality in hot loops, false sharing across threads,
  unnecessary heap allocations, hidden copies (missing `const&`, accidental value capture).
- **API & ABI** — needless surface area, leaky abstractions, exception-safety gaps.

Default to the simplest change that a reviewer would approve without a second round.

### Memory discipline

Heap allocation is expensive and, in hot paths, a code smell. Prefer, in order:

```
stack → arena → pool → frame allocator → heap
```

Reach for the heap last, and when you do in a hot path, justify it. Reuse scratch buffers across
frames instead of reallocating (ties into the Optimization workflow below).

## Where things live

```
Desert/Desert/Source/Engine/
  Core/        Application, entry, lifecycle          Graphic/     Vulkan API, Render, Systems, Materials, Environment
  ECS/         Entity, Components, System/*ECSSystem  Scripting/   Lua bindings
  Assets/      cooking, registry                      ShaderResources/  DSL parse → SPIR-V
  Audio/ Physics/ Animation/ Geometry/ Project/ Reflection/ Generated/
Desert/Common/Source/Common/Core/   ResultStr, JobSystem, Timestep — engine-agnostic primitives
Editor/Source/Editor/Panels/        ImGui panels (NodeGraph, SceneProperties, ...)
Tools/  + *.make                    DShaderTool, DShaderParser, PakTool, MeshSimplifier, HeaderTool, ...
Editor/Resources/Shaders/           .shader (DSL) + Common/*.glslh shared includes
```

Namespaces mirror the tree: `Desert::ECS`, `Desert::Graphic::Render`, `Desert::Core::Formats`,
`Desert::Editor`, `Common`. Types & methods PascalCase, members `m_`-prefixed.

## Core conventions (follow these, don't reinvent)

- **Errors: `Common::ResultStr<T>`, not exceptions**, for anything that can fail on data (bad
  shader, missing param, parse error). Return `Common::MakeError<T>("…")` / `MakeFormattedError` /
  `MakeSuccess(value)`; caller checks `IsSuccess()`. See `Common/Core/ResultStr.hpp`. Exceptions are
  for truly exceptional/programmer errors only.
- **Serialization: `rfl::json`** with `rfl::DefaultIfMissing` on read so old files stay loadable when
  you add fields. New serializable structs are plain aggregates (see `ShaderGraph::Document`).
  Adding a field must not break existing assets — default it.
- **Reflection/codegen**: the HeaderTool generates into `Engine/Generated/`. Don't hand-edit generated
  files; change the source annotation and regenerate.
- **Don't embed generated-language walls in C++.** Shader/GLSL boilerplate lives in shared `.glslh`
  includes configured via `#define`s (`Common/GraphVertex.glslh`, `TimeUB.glslh`), reusable by
  hand-written shaders and editable without rebuilding the editor. Generators emit *structure +
  the unique expressions*, nothing more. This was explicit review feedback — honour it.
- **Tests are green on both Debug and Release** before you call something done. CI runs both.

## ECS model

- Systems derive from `Desert::ECS::System` and implement
  `Update(entt::registry&, Graphic::Render::RenderCommandBuffer&, const Common::Timestep&)`.
- **Parallel systems**: override `CanRunParallel()` → `true` ONLY for read-only systems that make no
  structural registry changes and whose writes no other same-frame system reads. The scene runs
  consecutive parallel-capable systems concurrently on the JobSystem, **each with its own command
  buffer**; buffers execute in registration order afterward, so draw order stays deterministic.
  Anything touching Lua, input, physics, or components other systems consume MUST stay `false`.
  When in doubt, keep it serial — a wrong `true` is a data race, not a speedup.
- Prefer data-oriented iteration (`registry.view<...>()`) over per-entity virtual dispatch.

## Rendering

- Draw work is recorded into a `RenderCommandBuffer`, not issued immediately — keep it that way so
  parallel systems and deterministic ordering hold.
- **Editor Pass API**: external render passes (grid, collider debug draw) are injected as true 3D
  passes rather than special-cased in the core renderer. New editor-only visuals go through this API.
- Generic/data-driven materials (`MaterialComponent.ShaderName`) get engine-filled **opt-in uniform
  blocks by reflection name** — `TimeUB`, `DirectionLightsUB`. To feed a shader new engine data,
  add a named UB the reflection can find and fill it in `MeshRenderer`, don't hardcode a new path.
- Post/IBL/exposure are **compute** passes (bloom down/upsample, auto-exposure histogram, procedural
  sky bake, prefilter/irradiance). Prefer compute + shared LUTs over per-fragment recompute.
- **Vulkan lifetime discipline**: pipeline layouts, push-constant ranges, descriptor set layouts must
  outlive the pipelines that reference them. A dangling `VkPushConstantRange` already cost real FPS
  once (commit `868b6b2`) — own these in a stable container, never a temporary.
- **Depth is REVERSED-Z on `DEPTH32F`.** Every projection comes from `Core/Projection.hpp` (near and
  far handed to GLM swapped), depth clears to **0**, the default compare is `Greater`, and the far
  plane is 50 km. The shader DSL's `ZTest Less` still means "the nearer fragment wins" — that
  reinterpretation lives once in `PipelineCache`'s mapping, so **never mirror it by editing a
  `.shader`**, or the next shader written like all the others is invisible. **Shadow cascades keep
  standard Z on purpose** (an orthographic projection has no 1/z curve for the float exponent to
  cancel, so reversing buys exactly zero) and spell it with a raw compare op so they cannot silently
  follow the convention later.
- **Specialization constants are available** and need no permutation machinery: `ComputePipeline`'s
  spec takes an int32 vector, and the stage info is copied by value at creation. That is how a loop
  a scene never runs gets compiled away (the two-layer cloud march). Vulkan **silently ignores** a
  constant id the module does not declare, so a test must walk the SPIR-V and assert the id is
  really there — that failure mode is a correct, slow frame with no error anywhere.
- **`texture()` in a compute shader is undefined** — no derivatives, so no implicit level of detail.
  Always `textureLod(..., 0.0f)`. Three sites were relying on the driver happening to pick 0.

## Shader pipeline

- Authoring format is the **Desert Shader Language** (`.shader`): `Shader "Name" { Domain … Properties …
  State … Vertex{} Fragment{} Pass "Depth"{} }`. `Domain` ∈ `Surface | Terrain | Skybox | PostProcess`;
  only `Surface`/`Terrain` are user-assignable to a `MaterialComponent` (`ShaderProgramMeta::IsUserAssignable`).
- **Content-addressed SPIR-V cache** in `Cooked/ShaderCache` (commit `3248a0a`): compilation is keyed by
  a hash of the resolved source. When you change what a shader depends on (includes, defines), make sure
  it participates in the hash, or you'll serve stale SPIR-V. This hash-inputs-then-skip-work pattern is
  the model for any expensive, repeatable cook (meshes, LUTs, thumbnails).
- **Shader Graph** (`Editor/.../NodeGraph/`) compiles a node `Document` → `.shader` source. Node
  catalogue (`Specs()`) drives BOTH palette and compiler. Currently hardwired to `Domain Surface`;
  the intended direction is *domain as a document field over a shared node core* (Surface first,
  PostProcess next) — see the shader-graph discussion notes if extending it.

## Concurrency & the JobSystem

- `Common::JobSystem::Get()` is THE place for CPU-parallel work (asset cook, LUT gen, parallel ECS).
  `Submit` (fire-and-forget), `Async` (future), `ParallelFor` (blocks; the calling thread also works,
  so it can't deadlock a saturated pool). Workers = cores − 1.
- **Do not spawn ad-hoc `std::thread`/`std::async`** — that's exactly what JobSystem replaced, so
  thread count stays bounded and work is observable in one place.
- Jobs must NOT assume GPU / AssetManager / ECS access is thread-safe. Gather on a worker, mutate
  shared engine state on the owning thread.

## Optimization workflow

1. **Measure first.** Optick is wired in (`OPTICK_` scopes) and there's an in-viewport **Perf HUD**
   (FPS / frame-graph / top-scopes, commit `16421c4`). Add an `OPTICK_EVENT` around the suspect scope
   and read the top-scopes before changing code. Never optimize on a hunch.
2. **Cut per-frame allocations.** Reuse command buffers and scratch containers across frames; prefer
   `reserve`d/pooled storage over per-frame `new`/`vector` growth in hot loops.
3. **Cache expensive, repeatable work** with a content hash (the SPIR-V cache is the template): hash
   inputs → skip if unchanged. Applies to shader cooks, mesh cooks, thumbnails, LUTs.
4. **Parallelize the right things** via JobSystem / parallel ECS — CPU-bound, independent, read-only.
   Verify determinism (draw order) still holds.
5. **Batch GPU work**; avoid per-draw pipeline/layout churn and redundant descriptor updates.
6. **Re-measure** on Debug *and* Release — MoltenVK/Release behaviour differs; a Release-only
   regression (like the push-constant FPS bug) won't show in Debug.

## Anti-patterns (red flags in THIS engine)

If a change introduces one of these, stop and reconsider — it will fail review.

### Hardcoding content into a mechanism (the big one)

The engine provides **mechanism**; assets, scripts, and data provide **policy/content**. A module
meant to be dynamic, extensible, or data-driven must **never bake content-specific values or
identities into engine/module code**.

- ❌ A Lua script (or C++ system) that hardcodes the tunables of a specific thing — e.g. a "water"
  effect with literal wave speed, colour, and foam thresholds sprinkled through the code, or an
  `if (name == "water")` special case in the engine.
- ✅ The engine exposes a **generic** surface — material properties, shader params, a component with
  serialized fields, a Lua-configurable table — and the *water asset* supplies those numbers. Add a
  second water variant, or "lava", by authoring data, **not** by editing engine code.

Litmus test: *"Can a content author add the next variant without recompiling the engine?"* If the
answer is no and the module was supposed to be dynamic, you've hardcoded policy into mechanism.
Constants that are genuinely engine-invariant (a physical constant, a format magic number) are fine
— the rule is about **content** leaking into **engine**.

Where content belongs instead: `.shader` Properties, `MaterialComponent` fields, serialized component
data (`rfl::json`), Lua tables/config, project assets — all of which round-trip and are editable
without a rebuild.

### Engine-specific footguns

- **Ad-hoc `std::thread` / `std::async`** — use `JobSystem`; ad-hoc threads are exactly what it
  replaced. Unbounded thread count and invisible work is a red flag.
- **GLSL/codegen walls embedded in C++** — boilerplate goes in shared `.glslh` includes; generators
  emit only structure + unique expressions.
- **Exceptions for data errors** — a bad shader / missing param / parse failure returns
  `Common::ResultStr<T>`, it does not throw.
- **New singletons** — `JobSystem::Get()` is a deliberate global; don't add more. Pass dependencies
  explicitly.
- **`CanRunParallel() → true` on a system that writes shared state** — that's a data race dressed up
  as an optimization. Read-only, no structural changes, or it stays serial.
- **Vulkan handles owned by temporaries** — pipeline layouts / push-constant ranges / descriptor set
  layouts must outlive the pipelines using them (the `868b6b2` FPS bug).
- **Optimizing without a profile** — no `OPTICK`/Perf HUD evidence, no perf change.
- **Breaking asset compatibility** — adding a serialized field without a default breaks every old
  file; default it (`rfl::DefaultIfMissing`).
- **A mirror with only one reader.** The house pattern is a C++/GLSL pair asserted equal by a test,
  and it exists because two implementations of one quantity drift. But a mirror is a *guard*, not a
  ritual: if only one side ever reads the value, a second copy is the defect shape rather than
  protection from it. `CloudLayerAspect` is C++ only for exactly this reason, and says so.
- **A metre-era constant.** The world unit is the CENTIMETRE, and the migration left survivors — a
  grass draw distance of 45 (45 cm, so grass never drew), a blade width of 9 mm read as 0.09 mm, a
  35 m wind gust read as 35 cm, a preview projection with a 1 mm near plane. When you touch an old
  subsystem, **sweep it for lengths**: they are rarely alone, and a dimensionless ratio beside them
  is usually fine and should say so in a comment.

## Relations: when two numbers must agree, say so in code

Several defects in this engine were two individually-correct values that disagreed, so the codebase
now carries small headers whose whole job is to state a relation and let the engine complain:

- `CloudWeatherScale` — the weather tile against the layer's altitude (or a ground camera sees one
  cell overhead and a dozen at the horizon: an empty zenith over a dense band).
- `CloudMarchScale` — the march stride against layer thickness, swept over ELEVATION rather than
  evaluated at the zenith, because past the schedule's knee the stride grows faster than the chord.
- `CloudLayerAspect` — the layer's own proportions, cell width over thickness, which is what neither
  of the other two constrained: a 3.5 km deck under a 2.98 km cell passed both and looked like a
  ceiling overhead.

The pattern to copy: **derive the default from the relation**, let a preset choose inside a stated
range, and have the renderer log a warning that names the number the scene should have had. A
warning about physics ("a cloud narrower than it is tall is a convective tower, and a tower is
deep") is worth shipping; a warning about taste is not.

## When you finish

State plainly what you changed, how you verified it (which build config, tests, the actual
DShaderTool/engine output or Perf HUD numbers), and anything skipped. If a shader/material path
changed, confirm it round-trips through both `DShaderTool` and the engine.

**If the change alters what appears on screen, "builds and tests pass" is not verification** —
render a frame and look at it. The editor runs here and can shoot a scene to a PNG unattended; see
the `desert-engine-verify` skill for the command, the full test sweep, and why the defects that
have cost this project the most were all invisible to unit tests.

For what may not ship at all — TODOs, stubs, sliders that move nothing, an old path left alive
beside its replacement — and the definition of done, see the `desert-engine-contract` skill.
