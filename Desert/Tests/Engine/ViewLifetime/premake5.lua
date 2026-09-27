local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit under test is the view register, Engine/Graphic/ViewResources.cpp: which views are alive
    -- and which copies each one owns. It sits outside SceneRenderer.cpp because that file needs a
    -- VkDevice, so a view that is created and never destroyed would otherwise be assertable only by a
    -- human counting panels in a running editor. Plain C++, nothing else to link.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/ViewResources.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        -- <Editor/Core/SceneViewIdentity.hpp>. The editor is where views are opened and closed, and the
        -- naming of an open scene view is the half of "close a view" that a view count cannot check:
        -- ending the view while every surviving viewport's callback points at the wrong document is a
        -- green sweep and a broken editor. Header-only, no ImGui, nothing to link.
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
