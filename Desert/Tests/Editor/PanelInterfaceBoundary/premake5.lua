-- "The panel interface does not speak the toolkit's vocabulary, and this suite is the proof that it
--  can be compiled without it."
--
-- THE ABSENCE BELOW IS THE POINT OF THIS SCRIPT. There is no ThirdParty ROOT on the include path here,
-- and there is no loop over deps.DesertSpecific.IncludeDir (whose `base` entry is that root). That
-- directory is the only place <ImGui/imgui.h> resolves from, so this project compiles
-- Editor/Source/Editor/Panels/IPanel.hpp with Dear ImGui nowhere in sight: putting the include back
-- into the header does not merely redden a row in the suite, it stops the suite building. The rows in
-- panel_interface_boundary_test.cpp are what catches the other repair -- somebody adding the path back
-- here to make that failure go away.
--
-- Nothing from the engine or the editor is compiled or linked: IPanel.hpp and the three Editor/Core
-- headers it opens are header-only, and `Common` comes in for the logger and the event base class.
-- glm IS on the path (deps.Common.IncludeDir), because glm::vec2 is what the two virtuals return now --
-- the project's own pair of floats rather than a UI toolkit's.
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
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.Common.Defines) do
        defines { define }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

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
