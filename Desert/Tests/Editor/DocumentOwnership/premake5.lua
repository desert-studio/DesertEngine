-- "A tool and a document are owned by different things, and the difference is enforced."
--
-- The units under test are header-only and free of the renderer: Editor/Core/PanelRegistry.hpp carries the
-- container that refuses a document, Editor/Core/OpenDocuments.hpp the OWNER that holds one and refuses a
-- second for the same subject, Editor/Core/DocumentWell.hpp one VIEW over that owner, and
-- Engine/Core/RendererSlotPool.hpp the lease a closed document has to give back. They sit in headers for the
-- reason Editor/Core/SceneViewIdentity.hpp and Editor/Core/SubjectEditorRegistry.hpp do -- EditorLayer.cpp is
-- compiled by no suite (scripts/CI/UnreachedSources.sh), so anything assertable has to be lifted out of it.
-- Nothing to link from the engine or the editor; the ImGui and glm include paths are here because IPanel.hpp
-- declares ImVec2 members, not because any ImGui function is called.
--
-- ONE EDITOR .cpp IS COMPILED IN: SubjectEditorRegistry.cpp. The registry is the seam this suite is about
-- ("the set of open documents is the set of registered editors"), and its Register/Create carry the refusals
-- that make the rule hold -- an empty factory, a missing icon, a digest collision under one key. Those are
-- statements, not templates, so they cannot live in the header; compiling the one file is what makes them
-- assertable rather than merely written down. It pulls in Common's logger and nothing else.
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
