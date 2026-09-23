-- "A viewport that says which angle it is on must be able to say so for the angle it is actually on."
--
-- NO ENGINE AND NO DEVICE, and that is the point. The rule under test (PresetOfDirection) used to live
-- inside PresetOfCamera, where reaching it meant building an EditorCamera -- which needs Application,
-- a window and the Input singleton. While that was true the tolerance it carried was wrong for two of
-- the seven presets and nothing in the build could ask. The header now forward-declares the camera, so
-- this suite compiles the decision on its own.
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
        "%{wks.location}/Desert/Desert/Source",   -- <Engine/Core/CameraPitchLimit.hpp>
        "%{wks.location}/Editor/Source",          -- <Editor/Panels/ViewportPanel/ViewportCameraPreset.hpp>
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
