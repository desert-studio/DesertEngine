-- Workspace-wide settings shared by every project.
-- The workspace itself is declared here; per-configuration and per-platform
-- blocks live in Configurations.lua / PlatformWindows.lua / PlatformMacOS.lua.

workspace "Desert"
    -- The workspace is declared here but its files (Desert.sln / Makefile)
    -- belong at the repo root, same as before the config split.
    location ( _MAIN_SCRIPT_DIR )

    -- THREE CONFIGURATIONS, AND THE THIRD IS THE ONE A PLAYER GETS.
    --
    -- Debug and Release are both DEVELOPMENT builds: the frame capture, the profiler, the draw-call
    -- counter, the memory watch and the synchronous-load ledger are compiled into the player binary in
    -- both of them, because until now there was no configuration in which they were not. `Shipping` is
    -- that configuration. It is a BUILD CONFIGURATION and not a runtime flag on purpose: a flag can be
    -- turned back on by whoever runs the game, and code that is not in the binary cannot be.
    --
    -- See BuildScripts/Configurations.lua for what it means, and
    -- Desert/Tests/Runtime/ShippingBoundary for the census that keeps the list from growing silently.
    configurations { "Debug", "Release", "Shipping" }
    startproject "Editor"

    language "C++"
    cppdialect "C++20"
    -- Anchored to the repo root: relative paths here would resolve against
    -- BuildScripts/ (the directory of this script), not the workspace.
    targetdir ( _MAIN_SCRIPT_DIR .. "/build/Bin/%{cfg.buildcfg}" )
    objdir ( _MAIN_SCRIPT_DIR .. "/build/Intermediates/%{cfg.buildcfg}" )

    -- `externalanglebrackets "On"` stood here and MUST NOT COME BACK. It is MSVC's
    -- `/external:anglebrackets`, which calls EVERY `#include <...>` external regardless of where the
    -- header was found — and this codebase includes its own headers in angle brackets 2168 times
    -- against 260 quoted ones (`<Engine/...>`, `<Common/...>`, `<Editor/...>`). Paired with
    -- `externalwarnings "Off"` below it, it would have handed Windows a `/W4` that inspects `.cpp`
    -- bodies and is blind to every declaration — which is where `Bind` hides a virtual, where
    -- `override` goes missing and where a by-value return is spelled `const`. It cost nothing while
    -- `warnings "Off"` silenced everything anyway; the moment the level goes up it becomes a hole,
    -- and a hole that reports success. External-ness belongs to PATHS, and that is the line below.
    externalwarnings "Off"

    -- First-party code that happens to live under `ThirdParty/`. `desert-shared` is OUR submodule (the
    -- shared project format, and `ResultStr.hpp` itself), so its path must stay an ordinary include
    -- directory while everything else under `ThirdParty/` becomes external — otherwise the very header
    -- whose `[[nodiscard]]` this commit is switching on would be the one place the compiler stops
    -- looking. Workspace scope because it used to ride inside `Dependencies.Common.IncludeDir`, which
    -- every project loops, and that loop now declares its entries external.
    includedirs { _MAIN_SCRIPT_DIR .. "/ThirdParty/desert-shared/Include" }

    -- `warnings "Off"` stood here until 2026-09-06, and it was not a neutral default: premake expands
    -- it to `-w`, which reached **142 of the 144** generated makefiles in BOTH configurations. So every
    -- `[[nodiscard]]`/`NO_DISCARD` in this repository — 382 of them across 99 files, counted the day this
    -- was written; the Д31 census said 315 in 86 a week earlier — was decoration.
    -- Task И3 had already found that the guard itself was MISSPELLED in four places and nothing said so,
    -- which is the shape of the problem: with `-w` the compiler cannot even report that you asked it the
    -- wrong question.
    --
    -- "Extra" and not "High"/"Everything": premake maps it to `-Wall -Wextra` for clang and gcc, which is
    -- the level this commit brought to zero — 355 sites, measured in both configurations.
    --
    -- `-Werror` is NOT set, on purpose. The tree is silent under this flag as of this commit, and it has
    -- to STAY silent for a while under both compilers before a hard failure is a service rather than a
    -- hazard; the honest next step is `-Werror` on new files, not on all of them at once.
    warnings "Extra"

    -- WINDOWS GETS /W3 AND NOT /W4, AND THAT IS A MEASURED CHOICE RATHER THAN A SHRUG. `warnings "Extra"`
    -- means `/W4` on MSVC, and shipping it here would put this commit in exactly the state it exists to
    -- end: a flag switched on over a tree that is not quiet, on the half of CI nobody can see from a Mac.
    --
    -- The cost was estimated with the closest proxy available on this machine — the clang flags whose
    -- diagnostics ARE the /W4-only MSVC ones: `-Wconversion` and `-Wshorten-64-to-32` for C4244/C4267,
    -- `-Wsign-conversion` for C4245/C4389, `-Wshadow` for C4456-C4459. Over the same 672 translation
    -- units: **459 further sites**, 419 of them sign conversions. That is not a tail to clean up in the
    -- same change, and it is not something to leave screaming in a log.
    --
    -- /W3 is not a token level. Every diagnostic this task was actually about is at MSVC level 1 or 3:
    -- **C4834** (discarding a `[[nodiscard]]` value) is LEVEL 1, as are C4715 (not all control paths
    -- return a value) and C4700 (uninitialised local used); C4018 (signed/unsigned mismatch) and C4101
    -- (unreferenced local variable) are level 3. Each has a clang counterpart inside `-Wall -Wextra`
    -- — `-Wunused-result`, `-Wreturn-type`, `-Wsign-compare`, `-Wunused-variable` — and every one of
    -- those is at zero here, which is the evidence that /W3 should arrive quiet on Windows too. It is
    -- evidence and not proof: no Windows machine was available, so the first CI run is the measurement.
    --
    -- What /W4 would add on top is C4100 (unreferenced formal parameter) and C4189, both of which this
    -- commit has already cleared on the clang side, plus the 459 conversions. Raising it is a task with
    -- a number attached, not an oversight.
    --
    -- `warnings "Default"` is premake's spelling of `<WarningLevel>Level3</WarningLevel>` — verified by
    -- generating a vs2022 project, not assumed from the name.
    filter "system:windows"
        warnings "Default"
    filter {}

    -- THE 64-BIT-HOSTED TOOLS, BECAUSE THE 32-BIT LINKER RAN OUT OF ADDRESS SPACE, NOT THE DISK.
    -- Without this property MSBuild picks bin\HostX86\x64: a 32-bit link.exe and a 32-bit mspdbsrv,
    -- one per build and shared by every concurrent link, capped at 4 GB of address space. Debug
    -- objects carry their types inline (/Z7, BuildScripts/MSBuild/Ccache.targets) and every suite links
    -- Desert.lib, so each test link merges gigabytes of type records through that one server.
    -- Run 36032786406 (Windows Debug) is where it gave out: 321 suite links against 308 in the last
    -- green run 35998147036, Desert.lib 3289 MB against 3131, the 32-bit linker "ran out of heap
    -- space" three times, and eleven migrator suites died in LNK1201 ("check for insufficient disk
    -- space") and LNK1318 ("Unexpected PDB error; OK (0)"). The disk was not it: drive D had 176.5 GB
    -- free AFTER the failure and build\ held 40.3 GB. premake writes <PreferredToolArchitecture>x64,
    -- which moves link.exe and mspdbsrv to bin\HostX64 on CI and in a developer's Visual Studio alike.
    filter "system:windows"
        preferredtoolarchitecture "x86_64"
    filter {}

    -- Vendored code is not ours to fix, and its warnings would drown ours the moment they appeared.
    -- A path rule rather than `warnings "Off"` in each of the eleven ThirdParty project scripts,
    -- because the eleven do not cover it: `vk_mem_alloc.cpp`, `stb_image.cpp`, `stb_truetype.cpp`,
    -- `miniaudio.cpp` and `pl_mpeg.cpp` are vendored sources compiled INTO the Desert project, and a
    -- per-project setting cannot reach a single file inside one of our own targets. One rule reaches
    -- both, and a twelfth dependency is covered the day it is checked out rather than the day someone
    -- remembers.
    --
    -- The leading `**/` is load-bearing, and both of the two spellings a reader would reach for first
    -- match NOTHING from here: a bare `ThirdParty/**` is resolved against this script's own directory
    -- (`BuildScripts/ThirdParty`, where no source lives), and an absolute
    -- `_MAIN_SCRIPT_DIR .. "/ThirdParty/**"` does not match either, because premake compares the
    -- pattern against the file path AS THE PROJECT SCRIPT WROTE IT. Measured, not reasoned: with
    -- either of those two the generated makefiles carry zero per-file `-w`. If this rule ever stops
    -- matching, the tree does not go quiet — it goes LOUD, because vendored code starts reporting,
    -- and that is the direction this failure should point.
    filter "files:**/ThirdParty/**"
        warnings "Off"
    filter {}

    -- AND ONE VENDORED TREE THAT IS NOT UNDER `ThirdParty/`. LightweightVK (MIT, and it still carries its
    -- upstream licence header) was copied into the engine's own Vulkan utilities as
    -- `Graphic/API/Vulkan/VulkanUtils/lightweightvk`, so the rule above walks straight past it while it
    -- reports ten diagnostics we have no standing to fix. Its headers are included only by its own four
    -- sources, so a file rule reaches all ten. If it ever moves under `ThirdParty/` this block becomes
    -- redundant rather than wrong.
    filter "files:**/lightweightvk/**"
        warnings "Off"
    filter {}

    -- NO `MultiProcessorCompile` HERE, AND THE ABSENCE IS THE MEASUREMENT.
    --
    -- It was added on 2026-09-05 on a reading that looked airtight: `msbuild -m`, which CI passes,
    -- parallelises PROJECTS and not the files inside them, so Desert and Editor — which hold almost all
    -- of the code — were each compiled on one core. The step breakdown of Windows Debug backed it up
    -- (run 33958917578: Build 57.9 min, Run tests 32.9 min, everything else 2.7 min).
    --
    -- Then the runs came back and said the opposite. Windows Debug BEFORE the flag: 73, 73, 76, 84 min.
    -- AFTER it: 93.6, 94.4, 92.3. Roughly twenty minutes SLOWER.
    --
    -- The likely mechanism is the one this project spent the same day learning on its own hardware:
    -- `-m` and `/MP` MULTIPLY. Up to four project builds, each spawning up to four compilers, on a
    -- four-core hosted runner — sixteen processes for four cores, which is oversubscription, not
    -- parallelism.
    --
    -- The honest caveat: seven merges landed the same day and the suite count went 95 -> 99, so part of
    -- that twenty minutes is the tree growing. Two variables moved at once, which is exactly what this
    -- project's own rule forbids. So the flag is reverted to restore the known-good baseline, and the
    -- next Windows run answers the question with ONE variable changed: back near 73-84 means the flag
    -- was the cost; still near 93 means the tree grew and the flag was innocent.
    --
    -- THE ANSWER CAME BACK, AND IT ACQUITS THE FLAG. Run 33976407162, the revert, one variable moved:
    -- Windows Debug **93 min WITHOUT it** — indistinguishable from the 92-94 measured WITH it, and
    -- twenty minutes above the 73-84 baseline that predates both. So the twenty minutes is the TREE
    -- GROWING, not `/MP`, and the `-m` x `/MP` oversubscription story above is a plausible mechanism
    -- that this project never actually observed. It is written down because it was the reasoning at the
    -- time, not because it was confirmed.
    --
    -- What that leaves: the flag was never fairly measured in either direction. Re-adding it is now a
    -- legitimate experiment for task И1 rather than a mistake to avoid — one variable, one run, and if
    -- it is kept, drop `-m` from the msbuild invocation in the SAME change so the two cannot multiply.
    -- The ceiling is not the pressure it was: 93 min against `timeout-minutes: 150` is 62%, where the
    -- job had previously been cancelled twice at 90. The pressure is that the tree keeps growing.

    -- EnTT hands each component type a sequential index from ONE global counter, and without this the
    -- counter is a plain `id_type` incremented with `value++` (ENTT_MAYBE_ATOMIC, entt.hpp). Two threads
    -- first-touching two different component types can then be handed the SAME index and read each
    -- other's storage. Scene::PrepareComponentPools creates every pool serially so the engine's own
    -- collectors never reach that counter concurrently; this define is what makes the counter safe for
    -- every OTHER first touch (preview scenes, thumbnail scenes, scripts) and costs one atomic increment
    -- per component type per process.
    --
    -- WORKSPACE SCOPE IS THE POINT, not tidiness: the macro changes the TYPE of a shared static, so a
    -- project that compiled entt without it would disagree about that object's layout with every project
    -- that did. It must be all of them or none.
    defines { "ENTT_USE_ATOMIC" }
