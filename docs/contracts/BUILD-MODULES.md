# BUILD-MODULES — engine modules that compile separately (design + measurement, 11-21)

Owner 11-21: «стоит отделить тяжелые модули друг от друга ... сместиться только например вот на net/физику ...
Чтобы собиралось за минуты». Tail of BUILD-FAST-2. This document is the measurement, the target architecture and
the slice plan; it changes no code. The lead decides the option from section 9.

## 0. Answer in five lines

1. Today almost every engine header that is not a `.cpp`-private helper recompiles **370–570 of the 1108 engine
   objects** (Editor + engine + tools): leaf modules (Physics, Destruction, VFX, Water, Animation) are not leaves at
   compile time, because four hub headers (`Scene.hpp`, `ECS/Components.hpp`, `Runtime/ResourceRegistry.hpp`,
   `Assets/AssetManager.hpp`) include them and `EditorPCH.hpp` includes the hubs into all 298 Editor objects.
2. Even a one-`.cpp` edit in Engine pays **~22 s to re-archive `libDesertEngine.a` (1.9 GB of Debug objects)**,
   plus 4.4 s to link the Editor; the same 220 objects link into a `.dylib` in **3.0 s** (measured, section 3).
3. The module table (`cmake/DesertModuleTable.cmake`) already names 31 modules, but all legacy files share ONE
   include root (`Desert/Desert/Source`), so a forbidden include compiles; the only guard is the ModuleBoundary
   suite, and tests are off (owner 10-15). The boundaries exist on paper only.
4. Recommendation: **option (c)**, delivered in the order header cuts → per-module registration → physical
   Public/Private folders with closure-only include roots → shared libraries for Debug/Development builds.
   Shipping stays static (+ unity on Windows). Targets per slice in section 8.
5. Net and the new physics module are born under the rules of section 7: own folder with `Public/`+`Private/`,
   nothing in Engine/Scene/Components/EditorPCH includes them, their components and reflection register from
   their own module, their library is a separate dylib/DLL in dev builds. Touching them must not touch Engine.

## 1. How it was measured

* Build graph: `ninja -t deps` of `DesertEngine-BF2/build/cmake/macos-clang-ninja` (Debug, clang, ninja,
  `DESERT_UNITY=OFF`, tree at `6ae8a4f0a` = dev of 11-21 + WP-OPT-EVICT-BATCH), read-only. 1108 engine objects
  (ThirdParty excluded). "Objects" below = objects whose recorded depfile contains the header (exact count of
  what ninja rebuilds when the header's mtime changes; PCH members counted through the PCH).
* Direct include edges (who includes whom, for the root-cause chains): static scan of `#include` lines of the
  same tree.
* Seconds: `.ninja_log` of the same build dir, 4–5 builds. Per object: median of the samples longer than 0.3 s
  (shorter = ccache hit). Estimated wall time of "touch X" = max(Σ object CPU ÷ 10 cores, slowest object) +
  the archive time of the slowest touched library + 4.4 s Editor link. Object CPU is capped at 120 s, because three
  Editor objects have a single 520–604 s sample (ControlService.cpp, EditorPanels.cpp, LevelEditorCommands.cpp —
  section 3.3). The log was written under -j10 contention, so the seconds are an upper estimate; the object counts
  are exact. No build was run for this document.
* Archive vs shared library: section 3.2, timed by hand on copies in the scratchpad.

Reproduce: `ninja -C <dir> -t deps > deps.txt`, then the two scripts kept in
`DesertEngine-scratch/BUILD-MODULES-ARCH/` (an.py: deps + log → per-module CPU; an2.py: header fan-out / cost per file; inc.py: direct
include graph → `who`, `chain`, `reach`). `scripts/Dev/header_fanout.sh --objects <header>` gives the same object
count for one header.

## 2. Touch one file in module X → what recompiles (measured, 11-21)

| Module | File touched | Objects now | Modules hit | Est. wall now | Why (first hub on the chain) |
|---|---|---:|---:|---:|---|
| **Net** (new) | — | — | — | — | does not exist yet; must start at the section-7 target |
| PhysicsCore | `PhysicsCore/PhysicsWorld.hpp` | **411** | 12 | ~11 min | `ECS/Components.hpp` and `ECS/RagdollComponent.hpp` include it |
| PhysicsCore | `PhysicsCore/RagdollDesc.hpp` | 22 | 4 | ~2.8 min | only real users — this is what a leaf should cost |
| PhysicsCore | `PhysicsCore/PhysicsWorld.cpp` | 1 (2 s) | 1 | ~7 s | small archive + Editor link |
| Engine (physics) | `ECS/System/PhysicsBodyLifetime.hpp` | 14 | 2 | ~1.6 min | heavy includers (each ~50 s, they drag Scene) |
| Engine (physics) | `ECS/RagdollComponent.hpp` | 408 | 12 | ~11 min | lives inside `Components.hpp` |
| Hair | `Hair/GroomAsset.hpp` | 0 | 0 | ~5 s | the module is one generated TU; groom code has no consumer yet |
| Destruction | `Destruction/FractureCells.hpp` | **458** | 13 | ~11.4 min | `Scene.hpp → System.hpp → ResourceRegistry.hpp → FractureService.hpp → FractureAsset.hpp → FractureFormat.hpp → FractureBake.hpp` |
| Destruction | `Destruction/DestructionWorld.hpp` | 20 | — | ~2 min | real users |
| Destruction | `ECS/DestructibleComponent.hpp` | 409 | 13 | ~11 min | `Components.hpp` |
| VFX | `VFX/VFXClock.hpp` | 376 | 9 | ~10.4 min | `Scene.hpp → VFXWorld.hpp` (by value) |
| Water | `Water/GerstnerWaterWaves.hpp` | 453 | 13 | ~11.3 min | `ResourceRegistry.hpp → WaterWavesService.hpp` |
| Landscape | `World/Landscape/LandscapeData.hpp` | 438 | 12 | ~11 min | `Components.hpp` |
| AnimationCore | `Animation/Pose.hpp` | 489 | 13 | ~11.4 min | `Components.hpp → Animator.hpp`; `Components.hpp → Timeline/Sequence.hpp` |
| GeometryCore/Engine | `Geometry/Mesh.hpp` | 541 | 16 | ~12.3 min | `Components.hpp`, `Skeleton.hpp`; and Mesh.hpp itself drags the RHI (`GeometryPool.hpp → VertexBuffer.hpp`) |
| AssetCore | `Assets/AssetManager.hpp` | **571** | 18 | ~12.6 min | include cycle `AssetManager.hpp ↔ Mesh/MeshAsset.hpp`; `TextureAsset.hpp`; every service header |
| Renderer | `Graphic/SceneRenderer.hpp` | 99 | 5 | ~7.3 min | 61 Editor objects; median Renderer header = 100 objects |
| Renderer | `Graphic/ShaderDataType.hpp` | 573 | 18 | ~12.5 min | `VertexBuffer.hpp → Mesh.hpp` (RHI type in a world data header) |
| RenderCore | `Graphic/RDG/RDGBuilder.hpp` | 456 | 12 | ~11.2 min | `Scene.hpp → ExtensionPass.hpp → SystemRasterPass.hpp → RenderPassDeclaration.hpp` |
| Engine | `ECS/Components.hpp` | 408 | 12 | ~11 min | `Entity.hpp` includes it (389), `EditorPCH.hpp` (298) |
| Engine | `Core/Scene.hpp` | 372 | 10 | ~10.4 min | `EditorPCH.hpp` (298), `SceneRenderer.hpp` |
| Engine | `Core/Serialize/SceneFormat.hpp` | 390 | 11 | ~10.5 min | `Scene.hpp` |
| Engine | `Runtime/ResourceRegistry.hpp` | 447 | 13 | ~11.4 min | `ECS/System/System.hpp` (375), `EditorPCH.hpp`, 80 Renderer passes |
| Engine | any private `.cpp` | 1 (median 3.2 s) | 1 | **~30 s** | 22 s `ar` of libDesertEngine.a + 4.4 s link |
| Editor | `Source/EditorPCH.hpp` | 298 | 2 | ~6.9 min | the Editor PCH; includes AssetManager/Scene/Components/ResourceRegistry (lines 42-45) |
| Editor | `Editor/Core/CommandHistory.hpp` | 104 | 1 | ~4.5 min | widely used editor core header |
| Editor | median editor header | 4 | 1 | — | editor headers are mostly fine |
| Editor | any private `.cpp` | 1 (median 5.1 s) | 1 | ~40 s | 32 s `ar` of libEditorPanels.a (EditorLevel 22 s, EditorCore 9 s) + link |

Fan-out by module (median / p90 / max objects per header of that module): Common 458/1036/1090, AssetCore
70/601/649, GeometryCore 23/518/643, RenderCore 86/549/628, Renderer 100/120/574, Engine 14/422/698,
AnimationCore 417/465/490, Destruction 410/458/459, VFX 375/379/387, Water 450/454/454, PhysicsCore
412/458/458, Landscape 20/429/439, Foliage 23/59/410, Scripting 11/426/426, ScriptLuau 3/6/6, Editor 4/30/299.
ScriptLuau is the one module that already behaves as a separate module: nothing outside it includes its headers.

## 3. Where the time goes

### 3.1 CPU per module (full build, Σ object CPU, outliers capped)

| Module | Objects | Share of engine CPU |
|---|---:|---:|
| Editor (EditorPanels 133, EditorCore 59, EditorLevel 51, EditorImport 29, EditorWidgets 25, …) | 300 | ~49 % |
| Renderer | 116 | ~24 % |
| Engine | 220 | ~17 % |
| RenderCore | 96 | ~4 % |
| everything else (26 modules) | 376 | ~6 % |

The cost is not in the leaf modules: Physics, Destruction, VFX, Water, Hair, Foliage together are < 3 % of the
CPU. Their headers cost 10+ minutes only because the hubs carry them into the 300 Editor + 116 Renderer objects,
which are 5–20 s each (they parse ~360 engine headers apiece: `Scene.hpp` reaches 366 headers, `Components.hpp`
272, `EditorPCH.hpp` 367).

### 3.2 Static archives vs shared libraries (timed, 11-21)

| Step | Input | Time |
|---|---|---:|
| `ar qc` + `ranlib` libDesertEngine.a (ninja rule as generated) | 220 objects, 1.9 GB Debug | 22.0 s median of 4 ninja_log samples (67.9 s re-timed now, machine under another agent's build) |
| `clang++ -dynamiclib -undefined dynamic_lookup` | the same 220 objects | **3.0 s** → 154 MB dylib |
| libEditorPanels.a / libEditorLevel.a / libDesertRenderer.a archive | 133 / 51 / 116 objects | 32.0 / 22.3 / 12.8 s |
| Editor executable link (static) | all archives | 4.4 s |

An archive is rewritten whole (CMake: `rm -f; ar qc; ranlib`), so its cost grows with the module, not with the
edit. A shared library relink does not copy the DWARF (on macOS it stays in the `.o` files), so it is ~7× cheaper
and the executable does not relink at all when only a module's internals change. This is the measured case for
UE's "each module a shared library in editor/dev builds".

### 3.3 Single huge objects

`ControlService.cpp` (2462 lines), `EditorPanels.cpp`, `LevelEditorCommands.cpp` (402 lines!) each have one 520–604 s
sample; `MainMenu.cpp` 285 s, `PlaySession.cpp` 217 s, `DocumentHost.cpp` 187 s, `ComponentRegistry.cpp` (2436 lines)
116 s, `SceneRenderer.cpp` (5576 lines) 114 s, `SceneRendererFrameDeferred.cpp` 105 s. A 400-line file cannot cost
520 s on its own: these are template-instantiation or memory-pressure outliers and must be re-measured on a quiet
machine with `-ftime-trace` (slice BUILD-MOD-8). Anything over ~60 s sets the floor of "touch the hub" no matter how
well the graph is cut.

## 4. Root causes (which header drags which module)

1. **`Core/Scene.hpp` holds the world by value.** It includes `ECS/Entity.hpp` (→ `Components.hpp`),
   `ECS/System/System.hpp` (→ `ResourceRegistry.hpp` → every service → every asset format), `VFX/VFXWorld.hpp`,
   `Core/Serialize/SceneFormat.hpp`, `Assets/AssetManager.hpp` and `Graphic/ExtensionPass.hpp` (→ the RDG). Engine
   (L5) therefore includes Renderer-placed headers and every feature module above it.
2. **`ECS/Components.hpp` is an umbrella**: 3638 lines, 75 component structs, 57 includes, reaching 272 headers. It
   holds the components of Physics (`PhysicsWorld.hpp`), Animation (`Animator.hpp`, `Timeline/Sequence.hpp`),
   Landscape, Destruction, Foliage, the environment (`SceneEnvironment.hpp → SkyboxAsset.hpp → TextureAsset.hpp →
   AssetManager.hpp`), meshes (`Geometry/Mesh.hpp`). `ECS/Entity.hpp` includes it, so "I need an entity" = "I
   need every component".
3. **`Runtime/ResourceRegistry.hpp` includes its services** (Shader, Fracture, PhysicsAsset, WaterWaves:
   `ResourceRegistry.hpp:9-12`, BUILD-FAST-2 REMAINDER item 2) so that inline `GetXService()` compiles; and
   `ECS/System/System.hpp` includes the registry. Every system base drags Destruction/Water/Shader formats.
4. **`Assets/AssetManager.hpp ↔ Assets/Mesh/MeshAsset.hpp` include each other**; the AssetCore manager (L2)
   includes a typed Engine asset, and `Geometry/Mesh.hpp` includes `Graphic/GeometryPool.hpp` and
   `Graphic/VertexBuffer.hpp`: a world/geometry data header pulls the RHI.
5. **`Editor/Source/EditorPCH.hpp:42-45`** puts AssetManager, Scene, Components and ResourceRegistry into the
   precompiled header of all 298 Editor objects. With the hubs above, any change below them rebuilds the whole
   Editor, which is half the CPU.
6. **One include root for every legacy module.** `DesertModules.cmake` gives every engine target
   `DESERT_ENGINE_INCLUDE_DIRS` (the legacy root + all module roots, `DesertModules.cmake:265`); a closure-only
   variant (`desert_closure_includes`, :270) exists, but while the legacy modules share the legacy root any file
   can include any other. The table's `DEPS` are enforced by a test suite that does not run (tests off since 10-15).
7. **One global component/reflection registration.** `Core/Serialize/ComponentRegistry.cpp` (2436 lines, 116 s)
   names every component of every module, so it includes all of them, and adding a component anywhere edits
   an Engine file.

## 5. Target module graph

Layers stay as in `DesertModuleTable.cmake` (UE layering: Renderer above Engine, features as plugins over Engine).
Changes: physics gets a runtime module, Net is added, and every module gets a physical `Public/`/`Private/` split.

```
L0  Common
L1  CoreReflection  Projects  ImageCore  ApplicationCore  GeometryCore  Audio  Render2DCore
    PhysicsCore (Jolt wrapper: world, bodies, shapes, queries; no ECS, no assets)
    NetCore     (NEW: bit streams, packet/channel, connection, serialization of reflected values; no ECS)
L2  AssetCore  TextCore  DesertUI  AnimationCore
L3  Localization  ShaderCompiler
L4  RenderCore (RHI + Vulkan + RDG)
L5  Engine      (Scene = entity registry + subsystem collection; Entity; transform/hierarchy/camera/light/mesh
                 components; ISystem; service locator by type id; NO feature headers)
L6  Renderer  Media  Landscape  Destruction  VFX  Water  Hair
    PhysicsEngine (NEW, = UE PhysicsEngine/Chaos integration: physics components, body lifetime, character
                   movement, ragdoll, PhysicsAssetService, collision with Landscape via an Engine interface)
    Net           (NEW, = UE NetDriver/replication over Engine: replicated components, net subsystem, RPC)
L7  Foliage
L8  Scripting   L9 ScriptLuau
Editor modules (EditorCore, EditorWidgets, EditorPanels, EditorLevel, EditorImport) above all of them;
a feature's editor part is its own editor module (e.g. DestructionEditor) when it exceeds a few files.
```

### 5.1 Public / Private

* `Desert/<Module>/Source/<Module>/Public/...` — what other modules may include. `Private/...` — everything else,
  including `.cpp`, helpers, and headers used only inside the module.
* A module's compile include roots = its own `Public/` + `Private/` + the `Public/` of its DEPS closure. Nothing
  else. A forbidden include is a **compile error on both compilers**, not a test. (UE: Build.cs + UBT.)
* No umbrella headers (no `Desert.hpp`, no `Components.hpp`, no `Engine.hpp`). One header per type that others
  name; IWYU-strict public headers (forward declarations for pointers/references, `std::unique_ptr<T>` members with
  an out-of-line destructor).
* Engine headers never include a feature module. Engine reaches features only through interfaces it owns
  (`ISystem`, a typed service locator, `IComponentTypeRegistry`, `IPhysicsScene`-like seams) that the feature
  implements and registers at module start.
* PCH: per module, only `Common` + std + that module's own `Private` stable headers. `EditorPCH.hpp` keeps no
  Engine hub (BUILD-FAST-2 item 4).

### 5.2 Shared libraries in dev builds

* CMake option `DESERT_MODULAR` (default ON for Debug and Development/RelWithDebInfo presets, OFF for Shipping):
  each module = `SHARED` library; Shipping = `STATIC`, linked monolithic, unity ON on Windows as today.
* Exports: configure generates `<Module>/Public/<Module>API.hpp` with `DESERT_<MODULE>_API`
  (`__declspec(dllexport/dllimport)` on MSVC, `__attribute__((visibility("default")))` on clang; empty when
  static). Mac builds with `-fvisibility=hidden -fvisibility-inlines-hidden` too, so a missing export fails on
  Mac first instead of only on Windows (msvc-fix-prove-on-windows-first lesson).
  `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` is not used: Engine/Renderer in Debug exceed the 65 535 export limit of a DLL.
* Process-wide singletons live in exactly one module and are reached through an exported accessor (logger,
  JobSystem, asset manager, ImGui context in EditorCore). Header-defined statics (`inline` variables, function-local
  statics in templates) are forbidden in Public headers for anything that must be unique.
* EnTT across DLLs: `entt::type_index` uses a per-binary counter → mismatched ids across modules on Windows. Use
  `entt::type_hash` (name-based, stable across binaries) for every component storage, i.e. build with
  `ENTT_ID_TYPE` + named type hashes and `ENTT_API` set to the Engine export macro (EnTT's documented "DLL" mode).
  This is a precondition of BUILD-MOD-7 and must be proven on Windows first.
* Reflection/type registries: each module's `RegisterReflection_<Module>()` (already generated by
  DesertHeaderTool) and `RegisterComponents_<Module>()` (section 6) are exported entry points called by the module
  loader in table order. No static-initialiser self-registration (it is dropped by static linking in Shipping and
  ordered unpredictably across DLLs).
* Effect: a private `.cpp` edit = 1 object + one dylib relink (3 s for Engine, < 1 s for a feature module); the
  Editor executable does not relink. Live/hot reload of a module becomes possible later (UE Live Coding) but is not
  part of this plan.

## 6. Components and reflection per module

* Each component lives in the module that owns its behaviour: `PhysicsEngine/Public/RigidBodyComponent.hpp`,
  `Destruction/Public/DestructibleComponent.hpp`, `Landscape/Public/LandscapeComponent.hpp`, ... One header per
  component (or per tightly bound pair), REFLECT()/PROPERTY() as today.
* DesertHeaderTool already emits `RegisterReflection_<Module>()` per module. Extend it to emit
  `RegisterComponents_<Module>()` (storage, serializer, editor descriptor, default-construct, copy) from the same
  reflection data, replacing the hand-written `ComponentRegistry.cpp`. Engine keeps only the registry type
  (`ComponentRegistry` = map type-hash → descriptor) and the components it owns.
* Scene serialization (`SceneFormat`) works through the registry descriptors, so `SceneFormat.hpp` names no
  component and Scene stores no feature world by value (feature worlds become Scene subsystems created by their
  module: `VFXWorld` by VFX, `DestructionWorld` by Destruction, the physics scene by PhysicsEngine).
* Scene migrations (SceneMigrator) stay per format tag; a component moving module keeps its serialized name.

## 7. Rules for new modules (Net, PhysicsEngine) — from their first commit

1. Their own row in `DesertModuleTable.cmake` with an explicit FOLDER `Desert/<Module>/Source/<Module>` and
   `Public/`+`Private/`; no file of theirs under the legacy root `Desert/Desert/Source/Engine`, no placement regex.
2. DEPS: `NetCore → Common, CoreReflection`; `Net → Engine, NetCore`; `PhysicsEngine → Engine, PhysicsCore`
   (+ Landscape only if collision needs Landscape types; otherwise Landscape provides a heightfield through an
   Engine interface). Nothing below them names them: no Engine, Scene, Components, ResourceRegistry, EditorPCH,
   Renderer header includes a Net/Physics header. Checked at configure (BUILD-MOD-6) — until then by the lead with
   `header_fanout.sh --objects` on each landing.
3. Their components, systems, services and reflection register from their module
   (`RegisterReflection_<Module>`, `RegisterComponents_<Module>`, an `IModule::Startup()` that adds systems and
   services). No line in `ComponentRegistry.cpp` or in any Engine file for them.
4. Their PCH (if any) holds only std + Common + their own stable Private headers.
5. Public headers are IWYU-strict; no `inline` heavy functions in Public headers (logic in `.cpp`), no Jolt or
   socket/platform headers in Public (pimpl / opaque handles), so Jolt or a transport change recompiles only the
   module.
6. Editor UI for them lives in `<Module>Editor` (NetEditor, PhysicsEditor) or in their `Private/Editor` built into
   the editor-only library, never in EditorPanels.
7. Budget, checked on every landing: touch any Public header of Net or PhysicsEngine → **≤ 40 objects, ≤ 90 s**;
   touch a Private header/`.cpp` → **only the module's own objects + relink, ≤ 20 s**; no Engine or Renderer object
   in either set.
8. Windows: the module compiles in unity mode and as a DLL; every exported type carries `DESERT_<MODULE>_API`.

## 8. Slice plan

Each slice: branch `task/BUILD-MOD-n` from dev, ≤ 1 day, Mac build + Windows (MSVC unity) build, then landed.
The «frame» of these slices is a measurement printed by `scripts/Dev/header_fanout.sh --objects` (object count)
and one timed `ninja` of the Editor target after `touch` (wall seconds), recorded in
`DesertEngine-scratch/BUILD-MOD-n/timings.txt`, plus the regression scenario of BUILD-FAST-2 (WP_Protocol opens
and draws).

| Slice | Content | Target |
|---|---|---|
| **BUILD-MOD-1** (= BUILD-FAST-2 item 3) | `ResourceRegistry.hpp` includes no service (declarations + `template<class T> T& Get()`), the inline `GetXService()` users include their service; `System.hpp` stops including `ResourceRegistry.hpp` | touch `Destruction/FractureCells.hpp` → ≤ 25 objects; `Water/GerstnerWaterWaves.hpp` → ≤ 15; `Services/Shader/ShaderService.hpp` → ≤ 120; Editor rebuild after each ≤ 90 s |
| **BUILD-MOD-2** (= BUILD-FAST-2 item 4a) | `ECS/Components.hpp` → one header per component (75), placed in the owning module's folder; `Entity.hpp` includes no component; umbrella deleted, all consumers include what they use | touch `PhysicsCore/PhysicsWorld.hpp` → ≤ 30 objects; `ECS/RagdollComponent.hpp` → ≤ 25; `Animation/Pose.hpp` → ≤ 80; `World/Landscape/LandscapeData.hpp` → ≤ 40; `Components.hpp` does not exist |
| **BUILD-MOD-3** (= BUILD-FAST-2 item 4b) | `Core/Scene.hpp` diet: no `SceneFormat.hpp`, `VFXWorld.hpp`, `ExtensionPass.hpp`, `System.hpp`, `AssetManager.hpp` (forward decls, `unique_ptr` + out-of-line dtor, feature worlds as subsystems); `SceneFormat.hpp` names no component | `Scene.hpp` reaches ≤ 40 headers; touch `Core/Serialize/SceneFormat.hpp` → ≤ 20 objects; `VFX/VFXClock.hpp` → ≤ 20; `RDG/RDGBuilder.hpp` → ≤ 130 (RenderCore + Renderer only) |
| **BUILD-MOD-4** | `EditorPCH.hpp:42-45` drops AssetManager/Scene/Components/ResourceRegistry; `SceneWorkspace.hpp:61,73` fwd-decl `SceneRenderer`; cut the `AssetManager.hpp ↔ MeshAsset.hpp` cycle (manager is typeless, typed loads in typed headers); `Geometry/Mesh.hpp` without `GeometryPool.hpp`/`VertexBuffer.hpp` (render data moves to Renderer-side proxy) | touch `Assets/AssetManager.hpp` → ≤ 150 objects; `Geometry/Mesh.hpp` → ≤ 120; `Graphic/ShaderDataType.hpp` → ≤ 150; `Core/Scene.hpp` → ≤ 120 (no longer all 298 Editor objects) |
| **BUILD-MOD-5** | per-module component registration: DesertHeaderTool emits `RegisterComponents_<Module>()`; `ComponentRegistry.cpp` (2436 lines) reduced to the registry type; module startup calls the generated functions in table order | add a field to `DestructibleComponent` → ≤ 15 objects, none in Engine; `ComponentRegistry.cpp` ≤ 300 lines and ≤ 15 s |
| **BUILD-MOD-6** | physical modules: move Destruction, Water, VFX, Hair, Landscape, Foliage, Media, Scripting files into `Desert/<Module>/Source/<Module>/{Public,Private}` (rows' placement regexes deleted in the same commit); every engine target gets closure-only include roots (`desert_closure_includes` for all); a configure-time check (cmake reading `DesertModules.json`) replaces the disabled ModuleBoundary suite | a forbidden include (e.g. Engine → Destruction) fails compile on clang and MSVC; touch any Destruction Public header → ≤ 40 objects; Private → only Destruction objects |
| **BUILD-MOD-7** | `DESERT_MODULAR`: SHARED per module in Debug/Development, generated `<Module>API.hpp`, hidden visibility on Mac, EnTT named type hashes + `ENTT_API`, exported module entry points; Shipping static + unity unchanged | touch a private Engine `.cpp` → 1 object + libDesertEngine relink ≤ 5 s, Editor executable not relinked, total ≤ 15 s; private EditorPanels `.cpp` ≤ 15 s; Windows Debug Editor starts and opens WP_Protocol |
| **BUILD-MOD-8** | the huge objects: `-ftime-trace` on `ControlService.cpp`, `EditorPanels.cpp`, `LevelEditorCommands.cpp`, `MainMenu.cpp`, `PlaySession.cpp`, `SceneRenderer.cpp`, `SceneRendererFrameDeferred.cpp`; split by responsibility / move template instantiations out of headers | no engine/editor object > 30 s on an idle Mac; Σ Editor CPU −30 % |
| **BUILD-MOD-9** | the rest of Engine split along the same seams: Animation runtime (`AnimationEngine`), AI, GameplayDebugger, Media to their own modules; Editor feature editors into `<Module>Editor` libraries | median Engine header ≤ 14 objects stays; p90 Engine header ≤ 60 objects (today 422) |

After 1–4 the expected "touch a feature header" is tens of objects (≤ 1.5 min); after 7 a private edit anywhere is
≤ 15 s. "Собиралось за минуты" for any single-module edit is reached at slice 4; seconds at slice 7.

### Сценарии (frames) per slice

* BUILD-MOD-1 — Тогда [fanout_registry]: `header_fanout.sh --objects Engine/Destruction/FractureCells.hpp` ≤ 25,
  `.../Water/GerstnerWaterWaves.hpp` ≤ 15, `.../Shader/ShaderService.hpp` ≤ 120; Тогда [wp_protocol]: WP_Protocol
  opens and draws as on dev.
* BUILD-MOD-2 — Тогда [fanout_components]: `PhysicsWorld.hpp` ≤ 30, `RagdollComponent.hpp` ≤ 25, `Pose.hpp` ≤ 80,
  `LandscapeData.hpp` ≤ 40, no file `ECS/Components.hpp`; Тогда [wp_protocol]: as above, components serialize
  unchanged (scene opens with no load refusal in editor.log).
* BUILD-MOD-3 — Тогда [fanout_scene]: `Scene.hpp` reach ≤ 40 headers, `SceneFormat.hpp` ≤ 20, `VFXClock.hpp` ≤ 20,
  `RDGBuilder.hpp` ≤ 130; Тогда [wp_protocol]: as above, VFX emitters and destructibles of the scene present.
* BUILD-MOD-4 — Тогда [fanout_editor_pch]: `AssetManager.hpp` ≤ 150, `Mesh.hpp` ≤ 120, `ShaderDataType.hpp` ≤ 150,
  `Scene.hpp` ≤ 120; Тогда [wp_protocol]: as above.
* BUILD-MOD-5 — Тогда [fanout_registration]: field added to `DestructibleComponent` → ≤ 15 objects, 0 Engine;
  Тогда [details_destructible]: Details panel of a destructible shows the component and its properties.
* BUILD-MOD-6 — Тогда [boundary_error]: an injected `#include` Engine → Destruction fails configure/compile with
  the module names in the message (Mac + Windows log); Тогда [wp_protocol]: as above.
* BUILD-MOD-7 — Тогда [touch_engine_cpp]: `touch` one Engine `.cpp` → ninja log shows 1 object + 1 dylib link,
  ≤ 15 s wall; Тогда [win_modular]: Windows Debug Editor (DLLs) opens WP_Protocol and draws it.
* BUILD-MOD-8 — Тогда [time_trace]: the seven files ≤ 30 s each in `.ninja_log` of an idle build.
* BUILD-MOD-9 — Тогда [fanout_engine]: p90 Engine header ≤ 60 objects (an.py table).
* NET-ARCH / PHYS-GTA-ARCH first slices — Тогда [fanout_new_module]: any Public header of Net / PhysicsEngine
  ≤ 40 objects, 0 Engine/Renderer objects; Private `.cpp` → own objects + relink ≤ 20 s.

## 9. Options and the choice

| | (a) static libs, header cuts only | (b) (a) + shared libs in dev | (c) (b) + enforced graph + per-module registration |
|---|---|---|---|
| Touch a feature header | tens of objects after slices 1–4 | same | same, and it **stays** so (violations fail compile) |
| Touch a private `.cpp` | still 1 object + 12–32 s archive + 4.4 s link | 1 object + ≤ 3 s relink | same as (b) |
| Add a component to Physics/Net | edits Engine's `ComponentRegistry.cpp` (116 s TU + Engine archive) | same | module-only |
| Regression risk | the cuts re-grow (they already did: BUILD-FAST-2 measured fan-outs return within weeks) | same | guarded |
| Windows | unity unchanged | DLL exports, EnTT ids, singletons — real work | same as (b) |
| UE parity | no | partial | yes (Build.cs deps, Public/Private, *_API, per-module UHT) |

**Recommendation: (c)**, in the slice order of section 8. (a) alone is the first four slices of (c) and is where
the minutes come from, so nothing is lost by starting there; but without slices 5–6 the hubs regrow (the module
table already existed and did not stop `Scene.hpp` reaching 366 headers), and without slice 7 every single-file
edit in Engine/Editor keeps a 22–32 s archive floor. Where UE is worse: UE's DLL-per-module Debug editor has slow
startup (hundreds of DLLs) — we have ~31 modules, so this does not apply; we keep monolithic static Shipping like UE.

## 10. Open points for the lead

* The three 520–604 s samples (section 3.3) are single samples; re-measure on an idle machine before planning
  BUILD-MOD-8 effort.
* BUILD-MOD-7 on Windows depends on EnTT "DLL mode" working with our component storage; prove it on the Windows
  machine on a two-module toy before the full switch.
* `PhysicsCore` (L1) today already includes nothing above Common; `PhysicsEngine` is the new module the
  PHYS-GTA-ARCH slices should create, moving `Engine/Physics/*`, `Runtime/Services/Physics/*`,
  `ECS/System/{PhysicsBodyLifetime,KinematicBodies,RagdollLifetime,CharacterMovement,ProjectileMovement,LandscapeCollision,ColliderMesh}`
  and the physics components there.
