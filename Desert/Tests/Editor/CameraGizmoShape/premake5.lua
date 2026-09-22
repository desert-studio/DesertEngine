-- "A camera gizmo must say where the camera is, where it looks, and how wide -- at any distance."
--
-- ONE HEADER-ONLY SUBJECT, AND IT NEEDED NO DEVICE TO BE WRONG. Tools/CameraGizmoMath.hpp is the
-- shape `LightGizmoRenderer::RenderCameras` draws. It used to be built inline there, by inverting a
-- projection whose far plane was `cam.Near + 2.5f` -- a literal from the era when a world unit was a
-- metre. A unit is a CENTIMETRE, so the drawn volume was two and a half centimetres deep and the
-- lines never touched the camera at all. Reaching that code took a Scene, a camera, an ImGui draw
-- list and a person looking at the screen, which is why the defect lived where nothing could ask it
-- a question.
--
-- The assertions here are RELATIONS, and the one that fails on the old shape is the first: the edges
-- START at the camera. `RectangleIsTheCamerasOwnCone` is the NEGATIVE CONTROL -- the old inline
-- construction satisfied it too, so a suite made only of that would have been green over the defect.
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
        -- <Editor/Panels/ViewportPanel/Tools/GizmoTransformMath.hpp>, <Editor/Core/GizmoState.hpp>
        "%{wks.location}/Editor/Source",
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
