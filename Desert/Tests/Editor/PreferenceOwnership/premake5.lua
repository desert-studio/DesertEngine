local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The REAL preference store and the REAL gizmo seam, plus the project context that resolves
    -- ~/.desertengine. Half of what this suite asserts is about what reaches editor.json and comes
    -- back, so a mirror of the store would answer a different question than the one being asked.
    -- Nothing from the renderer or from ImGui is needed: the two files under test are std + rfl.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/EditorPreferences.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/GizmoState.cpp",
        -- К10's seam: the pure "user's answer minus what a viewport mode hides" function. It is a
        -- separate translation unit from ViewportPanel precisely so a test can link it — the panel
        -- itself needs ImGui, ImGuizmo and the whole renderer, and the arithmetic under test needs none
        -- of that.
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/ViewportModes.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Project/ProjectContext.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",   -- <Engine/Graphic/RenderConfig.hpp>, <Engine/Project/...>
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",          -- <Editor/Core/EditorPreferences.hpp>, <Editor/Core/GizmoState.hpp>
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",  -- rfl::fields<>, rfl::Generic, rfl::json
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

    -- DESERT_DEBUG_BREAK needs the platform macro; any engine header reaching DESERT_VERIFY fails to
    -- compile without it. main() also branches on it to set HOME the Windows way.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    links { "Common", "Optick" } -- Common's JobSystem registers its worker threads with Optick

    filter "system:not windows"
        links { "ReflectCpp" }

    -- Common contains Objective-C (the macOS file dialog), pulled in because EditorPreferences writes
    -- through Common::Utils::FileSystem, so the ObjC runtime + AppKit must link too.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
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
