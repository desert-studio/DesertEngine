-- "THE CHANNEL CAN ADDRESS EXACTLY WHAT THE PALETTE OFFERS -- NO MORE, AND NOTHING LESS."
--
-- The control channel runs the COMMAND PALETTE's entries, not a list of its own. Everything about the
-- design follows from that: a capability added for a person arrives for an agent with nobody wiring it up
-- twice, and there is no second execution path to fall behind the first.
--
-- Editor/Core/Control/ControlDispatch.hpp knows nothing about what a command DOES -- it takes any range of
-- things carrying a Group and a Label -- which is what lets the addressing rule be asserted without an
-- editor, an ImGui context or a GPU. Editor/Core/PreviewViewpoints.hpp is checked here too, because the
-- named viewpoints are the one capability that had to be DECOMPOSED to fit a palette entry.
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
        -- Ranking the near misses in a refusal. std-only, compiled straight in — the same way the
        -- FuzzyMatch suite next door takes it.
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/FuzzyMatch.cpp",
    }

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
