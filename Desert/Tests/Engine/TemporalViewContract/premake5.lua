-- TemporalViewContract (TAA1-C0): the per-view temporal foundation - resolution split, temporal method selection,
-- jitter, SceneViewState's frame protocol and history resets, MotionHistory, the RDG-FAULT1 policies of velocity
-- and history - pinned against the headers before the implementation exists. No GPU.
-- It links once TAA1's implementation adds Graphic/View/*.cpp below; until then the link failure IS the
-- contract's open state.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/View/ViewFrame.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/View/SceneViewState.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/View/MotionHistory.cpp",
        -- the graph the history registers into (no backend runs: Register / ExecuteReport only)
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGBuilder.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGCompile.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGPassBindings.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGAsyncFallbackLog.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    if deps.DesertSpecific.IncludeDir.Vulkan then
        externalincludedirs { deps.DesertSpecific.IncludeDir.Vulkan }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: Settings/Scalability (ResolveAntiAliasingForPath types), ResultStr (+ the JobSystem's profiler hooks).
    links { "Common", "Optick" }
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    filter "configurations:Debug"
        for name, path in pairs(deps.TestSpecific.Libraries.Debug) do
            links { path }
        end

    filter "configurations:Release"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end

    filter {}

print("Configured test project: " .. test_name)
