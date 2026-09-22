-- "The rail of the Clouds window names the six stages in build order, and each of them resolves to the
-- subject the editor for that kind of thing is registered under."
--
-- The unit under test is Editor/Panels/Clouds/CloudStages.hpp, which is header-only and deliberately free
-- of the engine: it carries the stage list, the order, and the mapping stage -> SubjectId, so that the
-- mapping can be asserted without a scene, an asset manager or a Vulkan device. CloudsPanel.cpp draws it
-- and is compiled by no suite (scripts/CI/UnreachedSources.sh), which is exactly why the rule was lifted
-- out of it.
--
-- ONE EDITOR .cpp IS COMPILED IN: SubjectEditorRegistry.cpp. The census this suite runs asks the real
-- registry whether a stage's subject type has an editor -- "a rail row is not a dead end" is a relation
-- between two things and asserting it against a stub registry would assert nothing. It pulls in Common's
-- logger and nothing else.
--
-- Nothing to link from the engine or the editor. glm is on the path because IPanel.hpp -- reached here
-- through SubjectEditorRegistry.hpp -- returns glm::vec2 from GetWindowPadding/GetDefaultSize. The TOOLKIT
-- is not: those two used to return ImVec2, which made this suite carry the ThirdParty root for a test that
-- calls no ImGui function. See Desert/Tests/Editor/PanelInterfaceBoundary.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files { test_files, "%{wks.location}/Editor/Source/Editor/Core/SubjectEditorRegistry.cpp" }

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
