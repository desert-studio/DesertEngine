-- "The scene shows the APPLIED state, the preview shows the WORKING copy, and the two coincide exactly
--  after Apply."
--
-- One relation, asserted from three sides. Two of them are pure value logic (MaterialEditStates.hpp is a
-- header over MaterialData, no GPU, no ImGui, no engine link); the third reads MaterialEditorPanel.cpp from
-- disk and pins WHICH material each audience is handed, for the same reason MaterialPreviewRoute does --
-- the panel owns a PreviewViewport, which owns a Scene and a SceneRenderer, and neither is constructible
-- without a Vulkan device.
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
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source", -- <Engine/Assets/MaterialData.hpp>
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",        -- <Editor/Panels/MaterialEditor/MaterialEditStates.hpp>
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
