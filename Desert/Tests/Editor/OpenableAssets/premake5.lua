-- "THE PALETTE OFFERS WHAT THE PROJECT HAS, NOT WHAT THE PRELOADER HAS GOT ROUND TO."
--
-- The command palette's `Open` group used to be built from the asset manager's CACHE. Measured through
-- the control channel, once per frame on this repository's own project, it goes 0 -> 106 -> 130 entries as
-- five separate startup stages fill that cache -- so for 3.3 s of every boot it successfully offered every
-- material and none of the twenty-four cloud assets. It is also how three files called `model.demat` came
-- to be three palette rows spelled identically.
--
-- Editor/Core/OpenableAssets.hpp takes the files it is given and hands back labelled entries: no
-- filesystem, no asset manager, no ImGui. That is what makes the two rules assertable at all --
-- EditorLayer.cpp, where the palette is assembled, is compiled by no suite (scripts/CI/UnreachedSources.sh).

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
