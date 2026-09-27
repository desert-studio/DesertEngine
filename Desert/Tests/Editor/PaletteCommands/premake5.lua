-- "A COMMAND THAT FAILED MUST NOT ANSWER LIKE ONE THAT WORKED."
--
-- PaletteCommand::Run returned `void`. The palette's dictionary is also the control channel's vocabulary,
-- so `run` over the socket answered {"ok":true} for a document that would not resolve, a scene that would
-- not save, an Apply that published nothing -- and the only trace was a log line the client was not
-- reading. The Ф4/Г13 shape: a result that exists, is known, and is dropped at the boundary.
--
-- Editor/Core/CommandPalette.hpp carries the outcome protocol and the ranking, and neither needs ImGui,
-- a GPU or an editor -- which is what lets this suite exist at all. CommandPalette.cpp (the drawing) is
-- compiled by nothing, and says so.

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
        -- What the palette's ranking calls, and what ResolveCommand's near misses call. std-only,
        -- compiled straight in — the same way the FuzzyMatch suite next door takes it.
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
