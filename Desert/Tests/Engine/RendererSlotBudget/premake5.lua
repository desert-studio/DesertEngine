-- "A user surface may take the last renderer slot; background work may not."
--
-- RendererSlotBudget.hpp is header-only and depends on nothing at all, which is the point: reaching a
-- sustained six-of-six by hand needs several scene views open at once, and those live behind a menu the
-- editor's control channel cannot click. Measured on this machine — five material documents opened in a
-- row touch 6/6 for one frame and fall back to 3/6 within eight seconds — so a FRAME cannot prove this
-- rule and a test can. Same shape, and the same reason, as RendererSlotPool being split out of
-- SceneRenderer in the first place.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files { test_files }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        -- <Engine/Core/RendererSlotBudget.hpp>. The ENGINE source root and not the engine LIBRARY: the
        -- header includes only <cstdint>, so this suite still links `Common` alone and still needs no
        -- device. Adding `Desert` here would link a Vulkan renderer to assert six comparisons.
        "%{wks.location}/Desert/Desert/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    links { "Common", "Optick" } -- Commons JobSystem registers worker threads with Optick

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
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
