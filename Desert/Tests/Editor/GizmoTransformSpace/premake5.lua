-- "A local-space drag must not do something the toolbar did not promise."
--
-- Two header-only subjects, and neither needed a device to be wrong:
--
--   * GizmoTransformMath.hpp -- the world-matrix -> local-TRS composition the object gizmo runs on every
--     manipulated frame. It lived inline inside GizmoController::RenderObject, which needs a Scene, a
--     camera, an ImGui draw list and a live ImGuizmo frame, so the one part that can be wrong on its own
--     was reachable only by launching the editor and dragging a handle by hand.
--   * GizmoState.hpp -- EffectiveSpace(), which is the ONLY thing that knows ImGuizmo discards the mode
--     argument while scaling. The toolbar button and the Manipulate() call both read it, so a test of it
--     is a test that the button cannot claim a space the handles will not honour.
--
-- GizmoState.cpp is deliberately NOT compiled in: its snap accessors reach EditorPreferences and the whole
-- settings tree, and nothing here calls them. Every Space entry point is inline in the header.
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
        -- <Editor/Panels/ViewportPanel/Tools/GizmoTransformMath.hpp>, <Editor/Core/GizmoState.hpp>
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
