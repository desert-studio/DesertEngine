-- "PLACING THE EDITOR CAMERA MUST NOT REQUIRE A CAPTURE FLAG."
--
-- `--camera` / `--look` are read only inside `shot.Active()`, so a developer who wanted the camera
-- somewhere and had no intention of taking a `--shot` had to launch the editor with a fictitious
-- `--shot --shot-frames 1000000` to unlock the placement. The mandatory step of a proof was being
-- performed by the flag family the control channel was built to replace.
--
-- Editor/Core/ViewportCameraProperties.hpp is the pose as a set of EDITABLE VALUES -- the channel's second
-- `properties`/`set` subject -- plus the one piece of arithmetic the placement rests on: where to aim so
-- the camera lands exactly where it was asked. Pure: no camera, no scene, no ImGui. That is what makes the
-- addressing and the arithmetic assertable, since EditorLayer.cpp is compiled by no suite
-- (scripts/CI/UnreachedSources.sh).

local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files { test_files }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
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

    links { "Common", "Optick" }

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
