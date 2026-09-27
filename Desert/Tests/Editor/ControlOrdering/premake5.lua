-- "A REPLY CANNOT OVERTAKE THE FRAME THAT PROVES IT."
--
-- The control channel's one promise: a reply leaves only after a frame that was PRESENTED and was rendered
-- with nothing outstanding. The obvious formulation -- "execute at the top of the update, answer after that
-- frame" -- is false, because EditorLayer queues several kinds of work between frames (document closes,
-- asset opens, scene loads, leaving Play) and a command can land in one of them. The gate is therefore
-- conditioned on QUIESCENCE, not counted in frames, and this suite is what says so out loud.
--
-- Editor/Core/Control/ControlPipeline.hpp is pure: no socket, no ImGui, no renderer, no clock. That is what
-- makes the ordering assertable at all -- EditorLayer.cpp, where the flags it reads actually live, is
-- compiled by no suite (scripts/CI/UnreachedSources.sh).
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
