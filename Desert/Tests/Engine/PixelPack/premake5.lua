-- "A CAPTURED PIXEL SAYS WHAT THE DEVICE SAID."
--
-- Engine/Graphic/PixelPack.hpp is the one place device bytes become the 8-bit RGBA a PNG writer wants, and
-- it is shared by both readbacks: the scene's offscreen image and the PRESENTED swapchain frame. They reach
-- it from different format vocabularies -- the engine's ImageFormat and a negotiated VkFormat -- so the two
-- mappings live at their call sites and the arithmetic lives here, once.
--
-- Written twice, the channel swizzle produces a capture whose red and blue are exchanged. That does not
-- read as a capture defect; it reads as a rendering defect, and gets reported as one. Hence one packer, and
-- hence a suite over it: it includes no Vulkan and touches no device, so what a captured pixel says is
-- decidable without a GPU.
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
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
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
