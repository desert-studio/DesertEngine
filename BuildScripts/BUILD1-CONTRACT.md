# BUILD1 — MSVC build time: measurement and contract (C0)

Status: CONTRACT, nothing implemented. Branch `task/BUILD1` (from `task/RDG-FINAL` eb177f8b8).
Every number below was read from the existing Debug tree `C:\DE\DesertEngine-6\build` (built 2026-10-06
03:33-04:02 on this machine, 3 other builds running) or produced by one single-file `cl.exe /Bt+
/d1reportTime` sample. Scripts: scratchpad `build1_measure.py`, `build1_measure_tests.py`, `build1_cl.bat`.
No project was built for this step.

## 0. What the brief assumed and what the tree says

| brief | measured | consequence |
|---|---|---|
| "no `pchheader` anywhere" | `Desert` HAS one (`Desert/Desert/premake5.lua:5-7`, `pch.hpp` = 14 std headers + `Common/Core/Core.hpp`, forced include); Common, Editor, Runtime, tools and all 366 suites have none | PCH work = widen Desert's, add Editor's and the runners'; not "introduce" |
| "each suite links the huge Desert.lib" | **1 of 366** links Desert.lib (`EngineHost`). 267 link Common.lib, 98 link neither; they compile engine sources into themselves | the cost is not the link of Desert.lib, it is **1,267 compiles of 221 distinct engine sources** (1,046 redundant), all without any PCH |
| "Debug link: add incremental" | Debug links are already `/INCREMENTAL /DEBUG` (366 suite `.ilk`, `Editor.ilk` 1.7 GB) | the link item is about `/DEBUG` kind and `/ZI`, not incremental |
| (not in the brief) | **No intra-project compile parallelism locally.** `Ccache.props` turns on `UseMultiToolTask` only when `DESERT_MSVC_CCACHE` is set (CI); `/MP` is absent on purpose (`Workspace.lua:143`). A local `msbuild -m` compiles Desert's 394 TUs **one at a time** in one `cl.exe` | the largest single local lever; added as step P0 |

## 1. Measured numbers (Debug, `C:\DE\DesertEngine-6\build`)

Compile — serial-per-project proxy = sum of consecutive `.obj` mtime deltas inside one contiguous build
(valid because each project's sources go to ONE `cl.exe`, which writes them in order):

| project | TUs | serial compile | median/TU | objects | notes |
|---|---|---|---|---|---|
| Desert (thin PCH) | 394 | **696 s** | 1.1 s | 3,066 MB; Desert.lib 3,627 MB | slowest: TextureSlot 17.7, SceneFormat 11.6, SceneRenderCollectors 11.2, ResourceRegistry 10.1 s |
| Editor (no PCH) | 161 | not contiguous in this tree (incremental gaps) | 2.5 s | 1,490 MB | median 1,007 headers per TU; `windows.h` family in **128/161** TUs; spdlog `fmt/format.h` in 143/161 |
| Common (no PCH) | 50 | 65 s | 0.2 s | 230 MB | Crc32c 34 s, WriteWatch 16.5 s: two outliers are half of it |
| 366 test suites | 1,799 obj | **3,956 s** + the first obj of each suite (not measurable by deltas) | 0.9 s/suite | **11,644 MB** obj, 12,420 MB pdb, 1,896 MB exe | 5,278 tests in 366 binaries |

Test-suite source duplication (from each suite's `CL.command.1.tlog`):

| origin | compiles | distinct | redundant |
|---|---|---|---|
| `Desert/Desert/Source` (engine) | 1,267 | 221 | **1,046** |
| `Desert/Tests` (the tests) | 437 | 437 | 0 |
| `Editor/Source` | 58 | 41 | 17 |
| ThirdParty / Tools / Common / Runtime | 37 | 23 | 14 |

Most re-compiled: Pose.cpp 28 suites, Skeleton.cpp 28, TimeModel.cpp 23, LandscapeData.cpp 23,
KeyInterpolation.cpp 22, Reflection.gen.cpp 19, ReflectionRegistry.cpp 19, CloudModellingVolume.cpp 19.

One TU sampled alone (`Engine/Animation/Pose.cpp`, Desert Debug flags, no PCH, loaded machine):
front end `c1xx` 14.3 / 26.3 / 21.9 s (`/ZI` / `/Z7` / no debug info), back end `c2` 2.4 / 1.6 / 1.7 s.
**The front end (parsing + template instantiation) is 85-94 % of a TU.** 563 headers, include time
6.2 s, class definitions 2.9 s. Objects: 3.9 MB (`/ZI`) / 5.1 MB (`/Z7`) / 2.4 MB (none).
The absolute seconds are inflated by the three concurrent builds; the split is what the sample is for.

Include graph (Desert `CL.read` tlog, headers beyond the PCH, by number of TUs):
glm core (`vec3`, `vec4`, `mat4x4`, `common`) 290-300 / 394; reflect-cpp (`rfl` + yyjson + ctre) 164;
GLFW `glfw3.h` + `GL/gl.h` 149; `<deque>` 182; `ContentRegistry.hpp` 163; entt `entt.hpp` (612 KB) 86;
`ECS/Components.hpp` (153 KB) 86; `vulkan_core.h` 57; `vulkan.hpp` family (~10 MB) 39; `vk_mem_alloc.h` 39;
`<ppltasks.h>` 38. The existing `Desert.pch` is 265 MB and is mapped by every one of the 394 TUs.

Link: `/INCREMENTAL /DEBUG` (full PDB) in Debug for Editor and every suite. Largest suite PDBs:
EngineHost 320 MB, AssetHandleStability 143 MB, PackagedContent 138 MB. Editor.pdb 823 MB, Editor.exe
110 MB. The "exe mtime minus newest own obj" link proxy is not trustworthy for incremental relinks
(median 1 s) and is not used for any decision below.

## 2. Plan, in order (each step is measured before/after on the same tree)

P0. Intra-project parallelism everywhere (not only under ccache).
P1. One test runner per layer, linking the libraries instead of recompiling their sources.
P2. PCH: widen Desert's, add Editor's and the runners'; standalone-TU census keeps it honest.
P3. Debug information and link settings.
P4. Unity: decision only (§6).
P5. Shared object cache: evaluation only (§7).

P1 goes before P2 because P1 deletes 1,046 of the compiles P2 would otherwise be tuned for.

## 3. P0 — parallel compile inside a project

`UseMultiToolTask=true` + `EnforceProcessCountAcrossBuilds=true` become unconditional project
properties (not `/MP`): one `cl.exe` per source, one machine-wide process budget shared by every
`msbuild -m` node, so `-m` x `/MP` cannot multiply (the hazard `Workspace.lua:143-178` describes).
`Ccache.props` keeps only what is ccache's (`CLToolExe`, `CLToolPath`, `TrackFileAccess`).

Wiring: premake 5.0-beta8 has no spelling for these (`Ccache.props:11`); emit them through
`premake.override(premake.vstudio.vc2010, "configurationProperties", ...)` in a new
`BuildScripts/MSBuildProperties.lua` included by `Workspace.lua`, so every generated `.vcxproj` carries
them — a developer's Visual Studio build included. Census: a premake-time check that every generated
`.vcxproj` has `<UseMultiToolTask>true` (the `BuildAllTests` Utility project excepted by kind).

Expected: Desert 696 s serial -> ~696/12 on this machine's 12 logical cores (~1-2 min when nothing else
builds), bounded by the slowest TU (17.7 s). CI already runs this way (ccache + MultiToolTask, run
35863890019: Build step Release 74.7 min, Debug 36.0); locally it is the missing half.
Verify: `msbuild Desert.vcxproj -t:Rebuild` wall time, before/after, same tree, nothing else building.

## 4. P1 — one test runner per layer

### Shape

`Desert/Tests/<Layer>/<Suite>/*.cpp` stays as it is on disk. The 366 `premake5.lua` files under
`Desert/Tests/**` are deleted. `Desert/Tests/premake5.lua` defines five projects:

| runner | sources | links |
|---|---|---|
| `CommonTests` | `Common/**/*.cpp`, `TestSupport/RunnerMain.cpp` | Common, GoogleTest, ReflectCpp, Optick |
| `EngineTests` | `Engine/**/*.cpp`, `TestSupport/**/*.cpp` | **Desert** + its link closure (as `EngineHost` links today), GoogleTest |
| `EditorTests` | `Editor/**/*.cpp` + the 41 Editor sources they need, compiled ONCE | Desert + closure, ImGui, GoogleTest |
| `RuntimeTests` | `Runtime/**/*.cpp` | as Runtime |
| `ToolsTests` | `Tools/**/*.cpp` + the 7 tool sources, once | Common/Desert as needed |

Engine sources are never compiled into a runner: `EngineTests` links the same `Desert.lib` objects the
editor ships (one source of truth, and the Desert PCH is reused). The "engine-without-Vulkan" property
that the hand-picked source lists gave some suites for free becomes an explicit include-graph census
(`Engine/Core`, `Engine/Animation`, ... include no Vulkan header) instead of a side effect of a link.

### Suite identity: the directory, read from `__FILE__`

gtest records each test's `file()`. `RunnerMain.cpp` maps a test to its suite = the directory under
`Desert/Tests/<Layer>/`. One source of truth: the same directory name the manifest and the reports
already use. The runner accepts `--desert-suite=<Suite>[,<Suite>...]`, translates it into the set of
test cases whose file lies in that directory, and refuses an unknown suite name (exit 2, the name in
the message) — an empty selection is an error, never "0 tests passed".

### How suites are run

- `build/TestManifest.txt` becomes `<Runner> <Suite>` per line, written by premake from the directory
  listing exactly as today (`Desert/Tests/premake5.lua:70-74`).
- `scripts/Windows/RunTests.ps1` keeps one PROCESS per suite (same parallelism `-Jobs`, same
  `build/TestScratch/<Config>/<Suite>` working directory, same per-suite xml/log, same timeout):
  `<Runner>.exe --desert-suite=<Suite> --gtest_output=xml:...`. Process-per-suite keeps today's
  isolation (static state, device init, cwd) — the merge changes the BUILD, not the run semantics.
- A single suite alone: `build\Bin\Tests\Debug\EngineTests.exe --desert-suite=AssetEviction`, or any
  gtest filter. `scripts/Dev/suite.sh <Suite>` resolves the runner from the manifest, builds that one
  runner, runs it with `--desert-suite`.
- CI (`.github/workflows/ci.yml`) and `run_tests.bat` change only through RunTests.ps1 / RunTests.sh.
- `--ListFile` shards keep naming suites.

### What must change in the suites (from the survey; list completed in REMAINDER)

| what | suites | change |
|---|---|---|
| own `int main` | 365 of 366 | deleted; `TestSupport/RunnerMain.cpp` is the only main |
| main with work beyond InitGoogleTest | EngineHost (`AddGlobalTestEnvironment(HostEnvironment)`), the cooked-mesh environment in `TestSupport/cooked_static_mesh.hpp:75,92`, CrashHandler (child mode, chdir to `argv[3]`, `crash_handler_test.cpp:448`) | global environments become suite-level `SetUpTestSuite` fixtures (a global env would boot the engine host for EVERY suite of the runner); the CrashHandler child mode becomes a runner dispatch `--desert-child=<name>` registered by the suite |
| same gtest test-suite name in two suites | `CloudNoiseContainer` (CloudNoiseSheet, CloudNoiseVolume), `ShaderRootFixture` (MeshVertexPath, PBRSceneFrame, ShaderCacheKey) | one shared fixture in `TestSupport/` or distinct names: gtest aborts when one test-suite name maps to two fixture classes, and two different classes with one name at namespace scope are an ODR violation the linker resolves silently |
| tests that `current_path(...)` | ~56 call sites (PackagedContent, AssetPathIdentity, WorldPartition, EngineShaderByGuid, PakChunks, MaterialDocumentOpen, MeshBinaryFormat, CrashHandler ...) | unchanged: process-per-suite keeps the scratch cwd; save/restore guards stay |
| tree location (walk up from cwd to `Editor/Resources/Shaders`) | many | unchanged (cwd is still `build/TestScratch/...` under the checkout) |
| `argv[0]` read in main | CrashHandler `crash_handler_test.cpp:440` (re-spawns itself, :69 CreateProcessW / :88 posix_spawn), AssetHandleStability `asset_handle_stability_test.cpp:1350`, CloudNoiseVolumeHandle `cloud_noise_volume_handle_test.cpp:152` | RunnerMain exposes the runner's own path to tests (one accessor in `TestSupport/`); CrashHandler's child is `<runner> --desert-child=crash` |
| `dependson` (build order on a tool) | 18 suites: AssetResolverCensus, CloudControlCensus, CloudProtocolScene, ComponentReflection, ConfigOwnership, FoliageTypeMigration, SceneCloudLayoutDefault, SceneDebugFields, SceneForeignKeys, SceneMeshGuidMigration, SceneMigratorWritePath, SceneSettingsHomes, SettingConsumers, SkyPresets, UIClipUndo, UIComponentRoundTrip, WorldSceneGenerator (+ the parent) | union moves onto the runner that holds them |
| extra links | ControlTransport (`ws2_32`, `advapi32`), UIScriptCollections (`Lua`) | onto their runner, with the reason |
| `TESTING` / `GTEST` in engine/editor/tool sources | none | linking the prebuilt `Desert.lib` (compiled without those defines) changes no engine code |

### Census that keeps it from regressing

- `TestRunnerLayout` (in `CommonTests`, text census): no `premake5.lua` under `Desert/Tests/<Layer>/`,
  no `int main` under `Desert/Tests/` except `TestSupport/RunnerMain.cpp`, no gtest test-suite name used
  in two suite directories, no class/struct at namespace scope outside an anonymous namespace in a
  test `.cpp` (ODR across a shared binary).
- RunnerMain self-check: every manifest suite selects >= 1 test (an empty suite is a failure).
- Mutations to prove it: add a `premake5.lua` to one suite dir; add an `int main`; duplicate
  `ShaderRootFixture`; misspell a suite in `--desert-suite` -> each must go red.

Expected: test compiles 1,799 -> ~437 test TUs + 41 Editor + 23 others (~500), every engine source
compiled once (in Desert, with PCH); 366 links -> 5; PDB 12.4 GB -> ~5 runner PDBs.
Verify: `msbuild -t:BuildAllTests` wall time + `build/Tests` size, before/after, same tree;
the sum of tests run per suite in the reports equals today's (5,278).

### Adding a suite (and converting one that arrives the old way)

A suite is a directory `Desert/Tests/<Layer>/<Suite>/` with `*.cpp` and nothing else: no `premake5.lua`, no
`int main`. Generation lists it in `build/TestManifest.txt` as `<Layer>Tests <Suite>` and compiles it into
the layer runner. A suite merged in from a branch that still has its own script converts in one minute:

1. delete `Desert/Tests/<Layer>/<Suite>/premake5.lua` and the suite's `int main` (RunnerMain is the main);
2. anything the script added beyond the template (an include dir, a library, a tool or editor source,
   a `dependson`) goes into `kRunners.<Layer>` in `Desert/Tests/premake5.lua`, with its reason; an
   engine source the script compiled is NOT copied: the runner links `Desert.lib`;
3. file-local types into `namespace { }`, and a gtest test-suite name no other suite of the layer uses.

`Desert/Tests/Common/TestRunnerLayout` names the suite and the rule it breaks if a step is missed. While
the Engine/Editor layers are converted (P1-B), a suite that keeps its `premake5.lua` AND its `main` still
builds as its own project (manifest line `<Suite> <Suite>`); one without the other is red.

Run one suite / all suites:

- Windows: `build\Bin\Tests\Debug\CommonTests.exe --desert-suite=Pak` (any gtest flag works with it);
  all: build the `BuildAllTests` target, then `run_tests.bat` (RunTests.ps1 reads the manifest).
- macOS: `CI=true premake5 gmake && scripts/Dev/suite.sh Pak` (resolves the runner from the manifest, builds
  only that runner, runs `--desert-suite=Pak`); all: `CI=true premake5 gmake`, then
  `make -C build/Projects config=debug BuildAllTests` (with your usual -j), then `scripts/MacOS/RunTests.sh "$PWD" Debug`.
- The ASan/CI shard planner (`scripts/CI/TestShards.py plan --expected build/TestManifest.txt`) and both
  runners take the suite list from the manifest only; nothing globs the output directory any more.

## 5. P2 — precompiled headers

### Selection rule (one rule, applied per project from its own `CL.read` tlog)

A header goes into a project's `pch.hpp` iff
1. it is std, Windows SDK, or `ThirdParty/` (stable), or one of the named rarely-changing engine
   foundation headers (`Common/Core/Core.hpp`, `Common/Core/ResultStr.hpp`, the Log header); AND
2. it is read by >= 1/3 of the project's TUs, or it is heavy (>= 500 KB of text with its closure) and
   read by >= 10 %.
NEVER: `Engine/Generated/*` (Reflection.gen.*), `ECS/Components.hpp`, any header under active
development — an edit to a PCH header rebuilds the whole project.

### Per project

- **Desert** (`Desert/Desert/Source/pch.hpp`, extend): std as today plus `<memory> <string_view> <span>
  <variant> <algorithm> <format> <chrono> <mutex> <atomic> <deque>`; glm core + `gtc/matrix_transform`,
  `gtc/quaternion` (290-300/394); reflect-cpp `rfl.hpp` + `rfl/json.hpp` (164/394, the heaviest template
  load); spdlog `fmt` via the Log header; `entt/entt.hpp` (612 KB, 86/394 — heavy rule); GLFW is NOT
  added (it drags `GL/gl.h` and `windows.h` into every TU — fix the leak instead, see below); Vulkan
  headers NOT added (57/394 and 39/394 for `vulkan.hpp`; ~10 MB would grow the 265 MB PCH every TU maps).
- **Editor** (new `Editor/Source/pch.hpp` + `pch.cpp`): std set as Desert; `imgui.h` (84/161);
  spdlog `fmt` (143/161); reflect-cpp (104/161); entt (89/161); glm; `IconsMaterialDesignIcons.hpp`
  (662 KB, 43/161 — vendored icon table, heavy rule).
- **Runners** (one `pch.hpp` in `Desert/Tests/TestSupport/`): `gtest/gtest.h` + the Desert set.
- **Common**: no PCH. 50 TUs, 65 s serial, two outliers (Crc32c 34 s, WriteWatch 16.5 s) are half;
  a PCH would not touch them. Revisit if Common's serial time passes ~3 min.

`windows.h` in 128/161 Editor TUs is a header leak, not a PCH candidate: find the engine/Common
header that includes it (the GLFW native / platform path) and confine it to the .cpp files that call
the OS. Putting `windows.h` in a PCH would hide the leak and its macros (`min`, `max`, `near`, `far`,
`CreateWindow`) from every translation unit's author.

### Wiring (premake)

```lua
-- each of Desert, Editor, the runners:
pchheader "pch.hpp"
pchsource "Source/pch.cpp"
forceincludes { "pch.hpp" }
filter "options:no-pch"            -- the census build
    flags { "NoPCH" }
    removeforceincludes { "pch.hpp" }
filter {}
```

### What keeps it honest: a standalone-TU census, not the forced include alone

The forced include keeps sources free of a `#include "pch.hpp"` line, so the PCH can be removed
without touching a source. But it also makes every PCH header silently available, so a source that
forgets `#include <glm/glm.hpp>` compiles. Two options were weighed:
- "sources include pch.hpp explicitly, no /FI" — same rot (the explicit line supplies everything) and
  every source gains a line that is not about its content; rejected.
- **chosen**: `/FI` + a `--no-pch` premake option and a CI step that compiles every TU of Desert,
  Editor and the runners WITHOUT the PCH, syntax-only (`clang -fsyntax-only` on the Linux/macOS leg, or
  `cl /Zs` on Windows), in parallel. It runs on every push; a missing include fails it with the file
  name. Mutation: delete one `#include <glm/...>` from a source that relies on the PCH copy -> red.

Expected: Desert per-TU front end (85-94 % of the TU) loses the glm/rfl/entt/fmt parse; Editor
loses the std+imgui+fmt+rfl parse of ~1,000 headers per TU. Verify: Desert and Editor rebuild wall
time before/after with P0 in place (P0 makes the comparison the parallel one users see), plus
`/Bt+` on Pose.cpp, TextureSlot.cpp, AssetFieldOpen.cpp with and without the new PCH.

## 6. P3 — debug information and link

| config | today | contract | why |
|---|---|---|---|
| Debug compile | `/ZI` (edit-and-continue), `/JMC`, `/RTC1` | `/Zi` (`editandcontinue "Off"`), keep `/JMC` `/RTC1` | Edit-and-continue is not used (CLI/agent workflow); `/ZI` pads functions, disables some codegen, serialises on `mspdbsrv` and is uncacheable. `/Zi` keeps a full PDB. Measured in P3, not assumed |
| Debug link | `/INCREMENTAL /DEBUG` (full) | **keep `/INCREMENTAL /DEBUG:FULL`**; no `/DEBUG:FASTLINK` | FASTLINK PDBs reference the `.obj` files instead of holding the types: DbgHelp-based readers (`CrashHandler`/`DesertCrashReporter` stack symbolisation, CR1b stack-overflow work) are not guaranteed to resolve them, a moved/cleaned intermediates dir breaks the PDB, and the CrashHandler suite asserts symbolised stacks. The incremental link already gives the relink saving FASTLINK would |
| Release compile/link | as today | unchanged except P0/P1/P2 | Release gate binaries stay what is tested |
| Shipping | `symbols "Off"`, `optimize "Full"` | **unchanged** | `ShippingSymbols.sh` contract (`Configurations.lua:34`) |

Verify: Desert rebuild time `/ZI` vs `/Zi` (same tree, P0 on); CrashHandler suite green and a
forced crash of the Editor Debug build symbolised by DesertCrashReporter.

## 7. Unity build — decision

**Not the local default, for Debug or Release.** Reasons, in order:
1. Debug: an edit recompiles a 12-source unity file, and the edit-compile loop is what Debug is for.
2. Release locally is the GATE build: it must report the code as written, one source per TU — a
   unity group hides a missing include and merges internal-linkage names (`UnityBuild.lua:10-15`, plus
   the opt-out list that already exists for exactly that).
3. After P0+P1+P2 the work unity removes (a header parse per source) is mostly gone, which is the
   same work the PCH removes; the two do not add.
CI keeps `--unity` as it is, and the decision is re-measured after P2: if CI Windows Release with
PCH + MultiToolTask + ccache is within ~10 % of unity, `--unity` and `UnityBuild.lua` are deleted
(one TU semantics everywhere). If it is not, it stays CI-only, with the numbers in `UnityBuild.lua`.

## 8. Shared object cache across worktrees — evaluation (no implementation)

What exists: ccache in masquerade mode for MSVC (`BuildScripts/MSBuild/Ccache.props/.targets`), CI only,
which already forces `/Z7` and the MultiToolTask. So the question is "ccache locally", not "sccache".

| | ccache (existing wiring) | sccache |
|---|---|---|
| new dependency | no (already in CI) | yes (contract §1.5: needs owner agreement) |
| MSVC PCH (`/Yc` `/Yu`) | does not cache PCH-using compiles in the shape this tree uses (counts them unsupported); to be confirmed from CI `ccache -s` after P2 | does not cache them either |
| `/Zi` `/ZI` | uncacheable -> needs `/Z7` (objects +30 % : Pose 5.1 vs 3.9 MB) | same |
| across worktrees | needs `CCACHE_BASEDIR` = the parent of the worktrees so absolute paths hash equal; PDB/`/FC` paths in objects differ per worktree | same |

Trade-off: a cache pays on a FRESH worktree or a branch switch (full rebuild); the PCH pays on every
build. They conflict exactly on Desert/Editor/runners (the PCH projects): with P2 in place the cache
can serve only non-PCH projects and the third-party libs, which are already cheap. Recommendation:
**do P0-P3 first; then measure a fresh-worktree build. If it is still dominant, the cache candidate is
the existing ccache wiring turned on locally for the non-PCH projects (ThirdParty libs, Common,
tools), not sccache.** Disk: the Debug tree is 11 GB intermediates + 23 GB bin + 29 GB tests today;
P1 shrinks it before any cache adds to it.
