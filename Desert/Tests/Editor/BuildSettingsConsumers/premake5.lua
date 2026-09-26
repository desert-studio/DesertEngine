local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Nothing is linked from the Editor: the two field lists come from rfl::fields<> over the HEADERS
    -- (PackageOptions and EditorPreferences), which is the same mechanism the values travel by, and the
    -- seam itself is checked by reading the panel and the packager as TEXT. That is what lets a suite
    -- about an ImGui panel run with no window, no GPU and no Editor link.
    files { test_files }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",   -- EditorPreferences.hpp -> Graphic/DebugViewState.hpp
        "%{wks.location}/Editor/Source",          -- EditorPreferences.hpp and Packaging/GamePackager.hpp
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- rfl::fields<>
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

    -- EditorPreferences.hpp reaches an engine header; DESERT_DEBUG_BREAK needs the platform macro.
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
