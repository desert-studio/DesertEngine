-- "A REQUEST IS UNDERSTOOD OR NAMED, AND A REPLY ALWAYS CARRIES AN OUTCOME."
--
-- The control channel's wire. Two rules, and both are rules this project has paid to learn elsewhere: the
-- parse is TOTAL, exactly as Editor/Core/CommandLine.hpp is for argv (an operation dropped in silence looks
-- to the client like an editor that did what it asked), and a response can never be a refusal with nothing
-- said, because Response has no way to express one.
--
-- Editor/Core/Control/ControlState.hpp is checked here too: the editor's state as JSON is the half of the
-- channel a client REASONS about, and a number in it that disagrees with the picture beside it is worse
-- than no number, because a report quotes it. It is pure for the same reason everything else here is --
-- EditorLayer.cpp, where the state actually lives, is compiled by no suite (scripts/CI/UnreachedSources.sh).
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

    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- requests and replies are rfl::Generic
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

    filter "system:not windows"
        links { "ReflectCpp" }
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
