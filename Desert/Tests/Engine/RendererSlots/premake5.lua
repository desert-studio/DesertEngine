local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit under test is header-only and free of the renderer: Engine/Core/RendererSlotPool.hpp is
    -- the whole of the renderer-slot accounting, on a plain bitmask. It sits outside SceneRenderer.cpp
    -- for the same reason GpuTimestampLayout.hpp sits outside VulkanGpuProfiler -- that file needs a
    -- VkDevice, so a slot that is leased and never returned would otherwise be assertable only by a
    -- human counting panels in a running editor. Nothing to compile from the engine, nothing to link.
    files {
        test_files,
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        -- <Editor/Core/SceneViewIdentity.hpp>. The editor is where slots are opened and closed, and the
        -- naming of an open scene view is the half of "close a view" that a slot count cannot check:
        -- returning the slot while every surviving viewport's callback points at the wrong document is a
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
