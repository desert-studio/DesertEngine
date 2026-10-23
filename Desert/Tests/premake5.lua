local currentDir = _MAIN_SCRIPT_DIR

os.mkdir(currentDir .. "/build/TestReports")

-- ── ONE TEST RUNNER PER LAYER (BUILD1 P1, BuildScripts/BUILD1-CONTRACT.md §4) ──────────────────────
--
-- A SUITE IS A DIRECTORY, `Desert/Tests/<Layer>/<Suite>/`, and the runner of its layer is ONE executable
-- (`<Layer>Tests`) that holds every suite of the layer and LINKS the layer's libraries. It replaces one
-- project per suite, each recompiling the engine sources it needed: 366 links and ~1,800 compiles of
-- mostly the same sources. The suite is chosen at run time, `<Layer>Tests --desert-suite=<Suite>`
-- (TestSupport/runner.hpp), and RunTests.ps1/.sh keep running one PROCESS per suite.
--
-- A suite has no premake5.lua and no `main`: its `*.cpp` (the suite directory itself, not below it) are
-- compiled into the runner, and TestSupport/RunnerMain.cpp is the runner's only `main`. What a suite
-- needs beyond that (an include directory, a library, a tool source) goes into its RUNNER's entry in
-- `kRunners` below, once, with the reason.
--
-- NO SUITE HAS A PROJECT OF ITS OWN. A premake5.lua in a suite directory fails generation with what to
-- do instead (a branch from another team that adds a suite the old way converts it in a minute:
-- BuildScripts/BUILD1-CONTRACT.md §4). Set-up a suite's main used to do is a SuiteEnvironment, a mode in
-- which the suite re-launches itself is a ChildEntry (TestSupport/runner.hpp); and
-- Desert/Tests/Common/TestRunnerLayout fails a suite that still defines a main.
local testsDir = currentDir .. "/Desert/Tests"
local kLayers  = { "Common", "Engine", "Editor", "Runtime", "Tools" }

local function DesertTestsCommonSettings(deps)
    kind "ConsoleApp"
    language "C++"
    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")
    -- TestSupport/*.hpp is included as "TestSupport/..." from every layer.
    includedirs { "%{_MAIN_SCRIPT_DIR}/Desert/Tests" }
    for _, p in DesertSortedPairs(deps.Common.IncludeDir) do
        externalincludedirs { p }
    end
    for _, p in DesertSortedPairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { p }
    end
    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        links { "Cocoa.framework", "Foundation.framework" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter "configurations:Debug"
        for _, lib in DesertSortedPairs(deps.TestSpecific.Libraries.Debug) do
            links { lib }
        end
    filter "configurations:Release"
        for _, lib in DesertSortedPairs(deps.TestSpecific.Libraries.Release) do
            links { lib }
        end
    filter {}
end

-- Desert and its link closure: what a runner that LINKS the engine needs (Tools, Engine, Editor). The engine
-- sources a suite tests come from Desert.lib, the same objects the editor ships; before BUILD1 the suites
-- compiled them (and vk_mem_alloc, VkBootstrap, stb, ImGui) one by one, so none of them is listed here.
local function DesertRunnerSettings(deps)
    files {
        -- Desert.lib registers the reflected types from an object nothing here references; this
        -- reference links it (see the file). Before BUILD1 the suites compiled Reflection.gen.cpp.
        "%{_MAIN_SCRIPT_DIR}/Desert/Tests/TestSupport/EngineReflectionLink.cpp",
    }
    includedirs(DesertModuleLibs.IncludeDirs())
    includedirs {
        -- Engine suites read header-only editor types (component editors' data, command records).
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/GLFW/include/",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/",
        -- Luau by module path (<VM/include/lua.h>): Engine/Scripting/Internal headers the script suites include.
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/luau",
    }
    -- Every engine third-party include (Jolt, Luau, stb, entt, meshoptimizer, OpenSubdiv, Vulkan...),
    -- from the engine's own list so the two stay in sync. The iteration skips the Vulkan keys when no SDK is set.
    for _, p in DesertSortedPairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { p }
    end
    defines { "USE_OPTICK=1", "OPTICK_ENABLE_GPU=0", "OPTICK_ENABLE_TRACING=0" }
    DesertModuleLibs.Link()
    links { "GLFW", "Optick", "MeshOptimizer", "OpenSubdiv", "ImGui", "Assimp", "OpenEXRCore", "Dav1d", "Opus", "Voro" }
    filter "system:windows"
        buildoptions { "/bigobj" }
    -- gmake does not link a static library's own dependencies transitively (Visual Studio does, through
    -- the project references), so on macOS the runner names what Desert.lib uses, exactly as
    -- Editor/premake5.lua does for the editor.
    filter "system:macosx"
        links {
            "Common",
            "Jolt",
            "LuauCodeGen",
            "LuauCompiler",
            "LuauAst",
            "LuauBytecode",
            "LuauVM",
            "LuauCommon",
            "ReflectCpp",
            "Cocoa.framework",
            "IOKit.framework",
            "CoreFoundation.framework",
            "CoreVideo.framework",
            "CoreMedia.framework",
            "AVFoundation.framework",
            "QuartzCore.framework",
            "Foundation.framework", -- Engine/Media (EngineHost's script)
        }
    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        links { deps.DesertSpecific.Libraries.Debug }
    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }
        links { deps.DesertSpecific.Libraries.Release }
    filter {}
end

-- What each runner adds to the common settings: the union of what its suites' own scripts carried
-- before BUILD1, each with the reason it is there.
local kRunners = {
    Common = function(deps)
        includedirs(DesertModuleLibs.IncludeDirs())
        includedirs {
            -- Rounding, ProductName, ReservedIdentifiers, TidyRegister-style text checks and AssetRenameMove
            -- read engine/editor headers that are header-only; nothing of Desert or Editor is linked.
            "%{_MAIN_SCRIPT_DIR}/Editor/Source",
            -- CrashHandler checks the packaged game's --crash-test parser (header-only RuntimeCrashTest.hpp).
            "%{_MAIN_SCRIPT_DIR}/Runtime/Source",
        }
        -- Optick: Common's JobSystem registers its worker threads with it. ReflectCpp: CanonicalText's
        -- writer reads and spells through yyjson, which ReflectCpp carries.
        links { "Common", "ReflectCpp", "Optick" }
        -- Subsystems: the header tool generates the sample owners' subsystem tables before the compile, and
        -- --check holds Owner/ to the tool's rules.
        local subsystems = "%{_MAIN_SCRIPT_DIR}/Desert/Tests/Common/Subsystems"
        dependson { "DesertHeaderTool" }
        prebuildcommands {
            DesertPlatform.BuiltToolPath("DesertHeaderTool")
                .. ' --templates "' .. _MAIN_SCRIPT_DIR .. '/Tools/DesertHeaderTool/Templates"'
                .. ' --check "' .. subsystems .. '/Owner"'
                .. ' --context "' .. _MAIN_SCRIPT_DIR .. '/Desert/Common/Source/Common/Core/Events"'
                .. ' --subsystems Sample SubsystemSamples::SampleOwner SampleOwner.hpp'
                .. ' "' .. subsystems .. '/Generated/SampleSubsystems.gen.cpp"'
                .. ' --subsystems SampleWorld SubsystemSamples::SampleWorld SampleOwner.hpp'
                .. ' "' .. subsystems .. '/Generated/SampleWorldSubsystems.gen.cpp"'
        }
        files { subsystems .. "/Owner/*.hpp", subsystems .. "/Generated/*.gen.cpp" }
        includedirs { subsystems .. "/Owner" }
    end,
    Runtime = function(deps)
        -- PackagedMount tests the packaged game's mount. Runtime is an executable, so the one source it
        -- needs is compiled here rather than linked.
        files { "%{_MAIN_SCRIPT_DIR}/Runtime/Source/PackagedContent.cpp" }
        includedirs {
            "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
            "%{_MAIN_SCRIPT_DIR}/Runtime/Source",
        }
        links { "Common", "Optick" }
    end,
    Tools = function(deps)
        DesertRunnerSettings(deps)
        -- PUC Lua: only the header tool's ModuleTable.cpp (ModuleBoundary, compiled below) uses it; scripts run
        -- on Luau. Named on every platform: Desert.lib no longer links it, so MSVC gets it from no reference.
        links { "Lua" }
        -- The tools are executables, so the sources their suites test are compiled here, once.
        files {
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/MigratorMain.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/SceneMigration.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/SettingsCanonical.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/UILift.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/MaterialAOParam.cpp", -- the retired OcclusionStrength rename (MAT-AO-ONEHOME)
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ClipInterpShift.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ClipMigration.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ClipGeneration3.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ClipLift.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ImportRecordSourceHash.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ShaderLocatorFollow.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/WorldGen/Source/WorldBuild.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/WorldGen/Source/WorldGenMain.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/CrashReporter/Source/CrashReport.cpp",
            -- WorldCells holds the world cook's cell partition (the file has no main of its own).
            "%{_MAIN_SCRIPT_DIR}/Tools/WorldCook/Source/WorldCookMain.cpp",
            -- HeaderToolChecks: the header tool's scanner and its COMPONENT(...) reader.
            "%{_MAIN_SCRIPT_DIR}/Tools/DesertHeaderTool/Source/HeaderScan.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/DesertHeaderTool/Source/AnnotationText.cpp",
            "%{_MAIN_SCRIPT_DIR}/Tools/DesertHeaderTool/Source/ComponentBlocks.cpp",
            -- ModuleBoundary: the module table's reader (it executes BuildScripts/DesertModules.lua with PUC Lua,
            -- which this runner links itself — the engine no longer does).
            "%{_MAIN_SCRIPT_DIR}/Tools/DesertHeaderTool/Source/ModuleTable.cpp",
            -- BuildScriptContract holds the editor's asset-reference scan to the build scripts.
            "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/AssetReferences.cpp",
            "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/AssetReferencesScan.cpp",
        }
        includedirs {
            "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source",
            "%{_MAIN_SCRIPT_DIR}/Tools/WorldGen/Source",
            "%{_MAIN_SCRIPT_DIR}/Tools/CrashReporter/Source",
            "%{_MAIN_SCRIPT_DIR}/Tools/WorldCook/Source",
            -- WorldCookMain.cpp opens its --project through the tools' shared ToolProject.hpp.
            "%{_MAIN_SCRIPT_DIR}/Tools/Shared",
            "%{_MAIN_SCRIPT_DIR}/Tools/DesertHeaderTool/Source",
            -- ReflectedFunctions: the generated registration includes its fixture as <Fixture/...>.
            "%{_MAIN_SCRIPT_DIR}/Desert/Tests/Tools/ReflectedFunctions",
            -- LuauRuntime: same arrangement, its own fixture (Fixture/LuauFixture.hpp).
            "%{_MAIN_SCRIPT_DIR}/Desert/Tests/Tools/LuauRuntime",
        }
        -- ReflectedFunctions: the header tool generates the fixture's reflection (FUNCTION thunks included)
        -- before the compile, exactly as Desert's prebuild generates the engine's. Its own force-link anchor:
        -- the engine's ForceLinkGeneratedReflection is Desert.lib's, which this runner also links.
        local reflectedFunctions = "%{_MAIN_SCRIPT_DIR}/Desert/Tests/Tools/ReflectedFunctions"
        dependson { "DesertHeaderTool" }
        prebuildcommands {
            DesertPlatform.BuiltToolPath("DesertHeaderTool")
                .. ' --templates "' .. _MAIN_SCRIPT_DIR .. '/Tools/DesertHeaderTool/Templates"'
                .. ' --reflect "' .. _MAIN_SCRIPT_DIR .. '/Desert/Tests/Tools/ReflectedFunctions" "Fixture"'
                .. ' "' .. _MAIN_SCRIPT_DIR .. '/Desert/Tests/Tools/ReflectedFunctions/Generated/FunctionFixture.gen.cpp"'
                .. ' --reflect-anchor ForceLinkReflectedFunctionFixture',
            -- LuauRuntime: the binder is exercised on a type the real tool reflected, never on a hand-built TypeInfo.
            DesertPlatform.BuiltToolPath("DesertHeaderTool")
                .. ' --templates "' .. _MAIN_SCRIPT_DIR .. '/Tools/DesertHeaderTool/Templates"'
                .. ' --reflect "' .. _MAIN_SCRIPT_DIR .. '/Desert/Tests/Tools/LuauRuntime" "Fixture"'
                .. ' "' .. _MAIN_SCRIPT_DIR .. '/Desert/Tests/Tools/LuauRuntime/Generated/LuauFixture.gen.cpp"'
                .. ' --reflect-anchor ForceLinkLuauRuntimeFixture',
        }
        files { reflectedFunctions .. "/Fixture/*.hpp", reflectedFunctions .. "/Generated/*.gen.cpp" }
        local luauRuntime = "%{_MAIN_SCRIPT_DIR}/Desert/Tests/Tools/LuauRuntime"
        files { luauRuntime .. "/Fixture/*.hpp", luauRuntime .. "/Generated/*.gen.cpp" }
    end,
    Engine = function(deps)
        DesertRunnerSettings(deps)
        -- Nothing of Editor/ or Tools/ is compiled here (TEST-LAYERS; TestRunnerLayout holds it): a suite that
        -- tests editor code is Editor/<Suite>, one that tests a tool's is Tools/<Suite>, in those runners.
        includedirs {
            -- One suite reads SettingConsumers' table.
            "%{_MAIN_SCRIPT_DIR}/Desert/Tests/Engine/SettingConsumers",
            -- Header-only tool cores the image and lattice censuses measure with.
            "%{_MAIN_SCRIPT_DIR}/Tools/ImageDiff/Source",
            "%{_MAIN_SCRIPT_DIR}/Tools/LatticePeak/Source",
        }
        externalincludedirs {
            -- FractureBake reads the vendored Voronoi cells (voro++) the engine's fracture bake is built on.
            "%{_MAIN_SCRIPT_DIR}/ThirdParty/voro++/src",
        }
        -- MediaPlayback / StartupMovie play the committed test clips.
        defines { 'DESERT_MEDIA_TEST_CLIP="' .. _MAIN_SCRIPT_DIR .. '/Desert/Tests/Data/Media/red_440hz_1s.webm"',
                  'DESERT_MEDIA_PATTERN_CLIP="' .. _MAIN_SCRIPT_DIR .. '/Desert/Tests/Data/Media/testsrc2_1080p_5s.webm"' }
    end,
    Editor = function(deps)
        -- EditorCore (Editor/premake5.lua) is every editor source but the entry point, built once as a static
        -- library, so a suite gets the editor code it tests by LINKING it, as the Editor executable does -- not
        -- from a list of editor sources compiled here a second time (BLD-EDITORCORE; the list was 67 files).
        -- Named before DesertRunnerSettings so the link line has it ahead of Desert and the libraries it uses:
        -- gmake resolves static libraries left to right.
        -- Whole, as the Editor links it: a suite reading the component-editor registry sees the editor's.
        links { "EditorCore" }
        DesertPlatform.LinkWholeArchive( "EditorCore" )
        DesertRunnerSettings(deps)
        files {
            -- PackagedContent: the player's packaged-content resolution (the Runtime executable's source).
            "%{_MAIN_SCRIPT_DIR}/Runtime/Source/PackagedContent.cpp",
        }
        includedirs {
            "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import",
            "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Panels/NodeGraph",
            "%{_MAIN_SCRIPT_DIR}/Runtime/Source",
            "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui",
        }
        externalincludedirs {
            "%{_MAIN_SCRIPT_DIR}/Editor/ThirdParty/assimp/include",
            "%{_MAIN_SCRIPT_DIR}/build/generated/assimp/include",
            "%{_MAIN_SCRIPT_DIR}/ThirdParty/openexr/src/lib/OpenEXRCore", -- <openexr.h>, texture import
            "%{_MAIN_SCRIPT_DIR}/build/generated/openexr/include", -- its generated config headers
            "%{_MAIN_SCRIPT_DIR}/ThirdParty/Imath/src/Imath",
        }
        links { "ImGuiNodeEditor" }
        -- EditorCore's face tracking builds against dlib when ThirdParty/dlib is present (Editor/premake5.lua).
        if os.isdir( _MAIN_SCRIPT_DIR .. "/ThirdParty/dlib" ) then
            links { "Dlib" }
        end
        filter "system:windows"
            -- The control socket (ControlTransport, ControlDispatch) is Winsock; the system file dialogs
            -- (Editor/Platform/Windows/DesktopPlatformWindows.cpp) are comdlg32/shell32/ole32 -- the same
            -- system libraries the Editor executable names, since both link EditorCore.
            links { "ws2_32", "advapi32", "comdlg32", "shell32", "ole32" }
        filter {}
    end,
}

-- TEST SOURCES OUTSIDE Desert/Tests THAT A SUITE ADOPTS (AdoptedTestSource, TestSupport/runner.hpp): suite -> its
-- files. They go wherever that suite is compiled -- its layer runner, and DevTests only when the suite is named
-- there: a runner holding the file without its adopting suite fails with "test outside Desert/Tests/<Layer>/<Suite>/".
local kAdoptedSources = {
    -- The launcher/engine project-format conformance suite (ThirdParty/desert-shared).
    ProjectFormat = { "%{_MAIN_SCRIPT_DIR}/ThirdParty/desert-shared/Tests/project_format_test.cpp" },
}

local deps           = dofile(currentDir .. "/Desert/Dependencies.lua")
local manifest_lines = {} -- "<Executable> <Suite>", one per suite
local test_projects  = {} -- every project RunAllTests depends on
local suite_home     = {} -- suite name -> { layer = <Layer>, dir = <its directory> }, for DevTests below

for _, layer in ipairs(kLayers) do
    local suiteDirs = os.matchdirs(testsDir .. "/" .. layer .. "/*")
    table.sort(suiteDirs)
    local configure  = kRunners[layer]
    local runnerName = layer .. "Tests"
    local converted  = {}
    for _, dir in ipairs(suiteDirs) do
        local suite = path.getname(dir)
        if os.isfile(dir .. "/premake5.lua") then
            error(string.format("Desert/Tests/%s/%s has its own premake5.lua: a suite is compiled into %s. "
                .. "Delete the script, delete the suite's main (set-up it did goes into a SuiteEnvironment, "
                .. "a child mode into a ChildEntry: TestSupport/runner.hpp), and move any source or include "
                .. "directory it added into kRunners.%s above (BuildScripts/BUILD1-CONTRACT.md §4)",
                layer, suite, runnerName, layer), 0)
        end
        table.insert(converted, dir)
        table.insert(manifest_lines, runnerName .. " " .. suite)
        suite_home[suite] = { layer = layer, dir = dir }
    end
    if configure and #converted > 0 then
        project(runnerName)
            DesertTestsCommonSettings(deps)
            files { testsDir .. "/TestSupport/RunnerMain.cpp" }
            for _, dir in ipairs(converted) do
                files { dir .. "/*.cpp" }
                files(kAdoptedSources[path.getname(dir)] or {})
            end
            configure(deps)
            removeconfigurations { "Shipping" }
        table.insert(test_projects, runnerName)
    end
end
for suite in pairs(kAdoptedSources) do
    if not suite_home[suite] then
        error(string.format("kAdoptedSources names %s, which is no suite directory Desert/Tests/<Layer>/%s", suite, suite), 0)
    end
end

-- DEVTESTS: A RUNNER OF ONLY THE SUITES AN AGENT IS WORKING ON (BLD-DEVTESTS). A layer runner compiles every suite
-- of its layer and links once per layer, so an agent touching one Engine suite and one Editor suite recompiled
-- the other suites' changed TUs and paid two of the biggest links of the build. `premake5 gmake
-- --dev-suites=A,B` adds one more project, DevTests, holding RunnerMain, those suites' sources and the runner
-- settings of every layer they come from -- the same configure functions as the layer runners, so a suite
-- compiles here exactly as in its own runner. It is a developer convenience and nothing else: not in the
-- manifest, not in RunAllTests, absent from every generation without the option, so the CI runners
-- (EngineTests, EditorTests, ToolsTests, ...) are generated byte for byte as before. Driven by
-- `scripts/Dev/suite.sh --dev-suites <Suite...>` and `build_quiet.sh ... --dev-suites=A,B`.
newoption {
    trigger     = "dev-suites",
    value       = "Suite[,Suite...]",
    description = "Also generate DevTests: one runner of only these test suites (agent builds; CI never passes it)",
}
if _OPTIONS["dev-suites"] then
    local dev_dirs, dev_layers = {}, {}
    for suite in _OPTIONS["dev-suites"]:gmatch("[^,%s]+") do
        local home = suite_home[suite]
        if not home then
            error(string.format("--dev-suites: %s is no suite directory Desert/Tests/<Layer>/%s", suite, suite), 0)
        end
        if not kRunners[home.layer] then
            error(string.format("--dev-suites: %s is in %s, a layer with no runner", suite, home.layer), 0)
        end
        table.insert(dev_dirs, home.dir)
        dev_layers[home.layer] = true
    end
    if #dev_dirs == 0 then
        error("--dev-suites names no suite", 0)
    end
    project "DevTests"
        DesertTestsCommonSettings(deps)
        files { testsDir .. "/TestSupport/RunnerMain.cpp" }
        for _, dir in ipairs(dev_dirs) do
            files { dir .. "/*.cpp" }
            files(kAdoptedSources[path.getname(dir)] or {})
        end
        for _, layer in ipairs(kLayers) do
            if dev_layers[layer] then
                kRunners[layer](deps)
            end
        end
        removeconfigurations { "Shipping" }
end

-- A SUITE THAT NEEDS A VULKAN DEVICE IS NAMED ONCE, HERE (it used to call `test_needs_vulkan_device` in its own
-- premake5.lua, which a suite no longer has). The names land in build/TestNeedsVulkanDevice.txt next to the
-- manifest, and that file is the ONLY place the CI learns it from: scripts/CI/TestShards.py plan leaves these
-- suites out of the shards (with a ::notice naming them) on a runner whose DESERT_*_VULKAN_RUNNER variable is
-- not 'true' -- the hosted macos-14 VM and windows-2022 have no device -- and plans them like every other suite
-- on one that is. The test itself never skips: on a deviceless machine it fails, as it should when someone runs
-- it there by hand. A name with no suite directory is an error, so a renamed suite cannot drop out silently.
local vulkan_device_suites = { "EngineHost", "RenderGraphVulkan" }
for _, suite in ipairs(vulkan_device_suites) do
    if #os.matchdirs(testsDir .. "/*/" .. suite) ~= 1 then
        error("vulkan_device_suites names " .. suite .. ", which is not exactly one suite directory", 0)
    end
end
table.sort(vulkan_device_suites)

-- ── THE TEST SUITES ARE NOT PART OF THE SHIPPING CONFIGURATION ──────────────────────────────────────
--
-- Shipping's defining property is that the development instruments are not in the binary. A test suite
-- IS a development instrument, and several of these suites exist specifically to hold one of the
-- instruments to its contract -- DrawCounterFunnel, MemoryDetector, GpuTimestampLayout,
-- SyncLoadChokepoint. Building them in the one configuration that removes what they test is a category
-- error with a cost attached in both directions:
--
--   * THE COST NOW, MEASURED ON WINDOWS RATHER THAN ESTIMATED (probe run 35633136353, the throwaway
--     workflow this decision was taken with): `msbuild Desert.sln -p:Configuration=Shipping` over the
--     WHOLE solution ran 1 h 36 min 41 s and then FAILED TO LINK. Without the suites the same
--     configuration builds green in 45 minutes (CI run 35635711944, Windows Shipping). So the suites
--     cost roughly an hour per platform per run and do not produce a binary at the end of it.
--
--     WHY IT FAILED IS THE SECOND HALF OF THE ARGUMENT, and it is not a defect to go and fix. Exactly
--     ONE of the 258 test scripts -- Desert/Tests/Runtime/ShippingBoundary's own -- names Shipping on
--     the filter that selects its libraries. The other 257 still read
--     `filter "configurations:Release"`, and a premake filter that matches no configuration
--     contributes nothing, so those 257 link no reflect-cpp and no gtest: the log is thousands of
--     LNK2001 lines for `rfl::json::Writer` and `yyjson`. Teaching 257 scripts to link a configuration
--     they should never be built in is work spent to make a category error compile.
--   * the cost later, which is worse: it puts a standing obligation on every future suite to compile
--     without the facility it is about, and the cheapest way to satisfy that obligation is to weaken
--     the test. A gate that pushes on tests in that direction is a gate that will eventually be paid.
--
-- Nothing is left unguarded by this. The half of the boundary that a test CAN prove is
-- Desert/Tests/Runtime/ShippingBoundary, which compiles no engine code at all (see its premake5.lua) --
-- it reads the sources as text, so its verdict is the same in every configuration and it runs on every
-- sweep. The other half needs a linked Shipping binary, which no test binary can produce, and that is
-- scripts/CI/ShippingSymbols.sh.
--
-- `removeconfigurations` and not `kind "None"`: the project must be ABSENT from the Shipping build
-- graph, not present-and-empty. A present-and-empty project is still a node the solution builds, still
-- a name in run_tests.bat's manifest, and still something a future `filter "configurations:Shipping"`
-- can accidentally bring back.
-- Applied per project in the layer loop above.

-- THE LIST OF EXPECTED TEST BINARIES IS A FILE, WRITTEN HERE, AT GENERATION TIME.
--
-- It used to be ~1900 `echo` lines in a postbuild event, one per line of a batch file this script
-- typed out character by character (see the postbuild block below for what is left of it). That
-- shape is why the runner was BLIND for months: `echo set ERROR=0>> file` was read by cmd as a
-- redirection of descriptor 0, the digit was eaten, and Windows Debug could not report a failure at
-- all before 2026-08-15. Every construct that survives quoting through Lua, through MSBuild's
-- Command element and through cmd is a construct nobody can read, and unreadable is how that defect
-- lived. The list is data, so it is written as data — no escaping, and `build/TestManifest.txt` can
-- be opened and diffed by hand.
--
-- THE LIST IS ALSO AN OBLIGATION, NOT A CONVENIENCE. The runner fails when a name here has no
-- binary. Globbing the output directory instead — which is what the unix runner still does — cannot
-- tell "this suite was deleted" from "this suite failed to link", so a suite that stopped building
-- would simply stop being run, silently. That is the same class of defect as the one above, and the
-- manifest is what closes it on Windows.
--
-- Each line is `<Executable> <Suite>`: the layer runner (or, for a suite not converted yet, its own
-- binary) and the suite directory it runs. Configuration-independent on purpose: Debug and Release build the same set of suites, so this is
-- written once at generation time and only the configuration travels through the postbuild below.
io.writefile(currentDir .. "/build/TestManifest.txt", table.concat(manifest_lines, "\n") .. "\n")
io.writefile(currentDir .. "/build/TestNeedsVulkanDevice.txt",
             table.concat(vulkan_device_suites, "\n") .. (#vulkan_device_suites > 0 and "\n" or ""))

group "Tests"
    project "BuildAllTests"
        kind "Utility"
        -- Same reason as the loop above: an aggregate over projects that do not exist in Shipping.
        removeconfigurations { "Shipping" }
        targetdir "%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}"
        objdir "%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}"


    project "RunAllTests"
        kind "Utility"
        -- Same reason as the loop above -- and one consequence worth naming: its postbuild is what
        -- writes run_tests.bat, so a Shipping build no longer produces a runner at all. That is the
        -- honest state. A runner that existed and ran nothing would be Ф4's shape: a silent pass.
        removeconfigurations { "Shipping" }

        -- These edges were added because this project's postbuild used to RUN the suite as well as
        -- write the runner, and without them MSBuild was free to schedule it alongside the test
        -- projects — and did: the Windows log shows "[ERROR] JobSystem.exe not found" interleaved
        -- with the linker still emitting ShadowCascades.exe. Every test was reported missing because
        -- it was still being compiled. (The same loop exists in BuildAllTests above with the
        -- dependson commented out, which is presumably where this went.)
        --
        -- The postbuild no longer runs anything (see the block below), so that symptom is now
        -- unreachable and the edges are not load-bearing for it. They are kept because they are what
        -- makes "RunAllTests" mean "every test suite is built": a Utility project with no edges is a
        -- node that claims a dependency it does not have, and the next person to put work back into
        -- this postbuild would inherit the 2026-08 defect all over again.
        for _, projectName in ipairs(test_projects) do
            dependson(projectName)
        end

    if os.target() == "windows" then
        -- THREE LINES OF PAYLOAD, AND THE SHAPE OF ALL THREE IS DICTATED BY WHAT cmd EATS.
        --
        -- What this writes is a shim: it names the configuration that was just built and hands the
        -- work to scripts/Windows/RunTests.ps1, which is committed, readable, and testable. The
        -- ~1900 echo lines it replaces built the whole runner out of cmd fragments that had to
        -- survive three levels of quoting; the manifest note above records what that cost.
        --
        -- NO PAYLOAD LINE MAY END IN A DIGIT. `echo ... exit /b 1>> file` does not write the
        -- digit: cmd reads `1>>` as a redirection of descriptor 1 and the `1` never reaches the
        -- file. That is character-for-character the 2026-08-15 defect — `set ERROR=0>>` losing its
        -- zero and leaving Windows Debug unable to report a failure — and the first draft of THIS
        -- block reintroduced it while writing the exit check that was supposed to prevent it.
        --
        -- SO THERE IS NO EXIT CHECK, AND THAT IS THE SAFER OF THE TWO OPTIONS. A batch file that
        -- ends without `exit` returns the exit code of its last command, and the last command here
        -- is pwsh. The alternatives all need a literal digit or a `%`: `exit /b 1` is the digit
        -- above, and `exit /b %ERRORLEVEL%` puts a `%` inside an MSBuild <Command>, where `%XX` is
        -- an escape and `%(` is item metadata. Removing cmd from the verdict entirely beats both —
        -- RunTests.ps1 decides, and cmd only carries the number out.
        --
        -- `-Config %{cfg.buildcfg}` is last on that line for the same reason: it ends in a letter.
        --
        -- `pwsh` BY NAME, NOT BY PATH, AND WITH WINDOWS POWERSHELL 5.1 BEHIND IT. The runner image installs PowerShell 7 at
        -- `C:\Program Files\PowerShell\7\pwsh.EXE` — visible in the Windows job's own log, because
        -- ci.yml's Vulkan steps use `shell: pwsh` and GitHub resolves that name off PATH in the same
        -- job. Hard-coding the directory would pin us to a `7` that will become an `8`.
        --
        -- A developer machine usually has no PowerShell 7 at all, and there the shim used to die
        -- with "'pwsh' is not recognized" (errorlevel 9009) before reaching the one script that
        -- owns the verdict, so the file CI generates could not be run locally. Windows PowerShell
        -- 5.1 ships with the OS and runs RunTests.ps1 unchanged, so `where /q pwsh` picks the
        -- interpreter and BOTH branches invoke the SAME script with the SAME arguments.
        --
        -- `where /q` and not `where pwsh >nul`: /Q prints nothing and answers in the exit code, so
        -- the probe line carries no redirection that would have to survive echo, MSBuild's XML and
        -- cmd's parser. `if errorlevel 1 (...) else (...)` and not `pwsh ... || powershell ...`:
        -- with `||` a FAILING TEST RUN under pwsh would re-run every suite under 5.1 and report the
        -- second verdict. The `if` line ends in `)`, which keeps the no-trailing-digit rule above.
        --
        -- The failure direction is still right: with neither interpreter on PATH cmd answers 9009
        -- and the step goes red. A missing runner cannot come back as a pass, which is the only
        -- property this shim absolutely must have.
        --
        -- NOTHING RUNS THE TESTS HERE. Until 2026-09-08 the last postbuild command was
        -- `call run_tests.bat`, so the suite ran once inside `msbuild` and then AGAIN in the CI job's
        -- own "Run tests" step. Measured on run 34223623196: the in-build run cost 28 min 39 s of a
        -- 62.7-minute Windows Debug build step, and the step that follows it repeated the same work
        -- in 24 min 02 s. Writing the runner and running it are different jobs, and the CI step is
        -- the one that owns the verdict — it is where the exit code is read and where the reports
        -- are uploaded from. (Release paid 2 min 20 s for the same duplicate.)
        -- The runner's path INSIDE run_tests.bat is %~dp0 (the .bat's own folder, the repository root),
        -- written as %%~dp0 so the postbuild's cmd leaves it for the .bat to expand. It was
        -- %{_MAIN_SCRIPT_DIR}, which premake writes RELATIVE to the project file: fine for the postbuild
        -- itself (it runs in the project folder), wrong inside a file that CI runs from the root. BLD1
        -- moved the project files to build/Projects and every Windows test step answered "'../..\scripts
        -- \Windows\RunTests.ps1' is not recognized" (int/3, run 36320005709).
        postbuildcommands {
            "if exist \"%{_MAIN_SCRIPT_DIR}\\run_tests.bat\" del \"%{_MAIN_SCRIPT_DIR}\\run_tests.bat\"",

            "echo @echo off > \"%{_MAIN_SCRIPT_DIR}\\run_tests.bat\"",
            "echo where /q pwsh>> \"%{_MAIN_SCRIPT_DIR}\\run_tests.bat\"",
            "echo if errorlevel 1 (powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%%~dp0scripts\\Windows\\RunTests.ps1\" -Config %{cfg.buildcfg}) else (pwsh -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%%~dp0scripts\\Windows\\RunTests.ps1\" -Config %{cfg.buildcfg})>> \"%{_MAIN_SCRIPT_DIR}\\run_tests.bat\"",
        }
    end

    -- NO `else` BRANCH, AND ITS REMOVAL IS A DELETION OF DEAD CODE, NOT A BEHAVIOUR CHANGE.
    --
    -- There used to be one here that read `postbuildcommands { bash scripts/MacOS/RunTests.sh ... }`
    -- above a comment claiming it "does the same job as the generated run_tests.bat". It never ran a
    -- single test. RunAllTests declares no `targetdir`, so gmake2 leaves both TARGET and TARGETDIR
    -- undefined in RunAllTests.make; `all: $(TARGETDIR) $(TARGET)` collapses to a rule with no
    -- prerequisites and the POSTBUILDCMDS block is never reached. Verified by running it rather than
    -- by reading it — `make -f RunAllTests.make config=debug` prints nothing and exits 0 — and
    -- corroborated on CI, where the macOS jobs show exactly ONE "===== Starting Tests =====" and its
    -- timestamp is the start of the workflow's own "Run tests" step to the second.
    --
    -- So the unix side has always been driven by ci.yml calling scripts/MacOS/RunTests.sh directly,
    -- which is the right place for it, and putting it back in a postbuild would only re-create the
    -- duplicate run that the Windows branch above just stopped paying for.


print("\n=== Test Configuration ===")
print(string.format("%d suites in %d test projects (build/TestManifest.txt)", #manifest_lines, #test_projects))
print("Test reports will be saved to: " .. currentDir .. "/build/TestReports")
