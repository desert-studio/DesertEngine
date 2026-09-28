-- RenderGraphVulkan: the render graph's Vulkan executor on a REAL device (headless: no window, no
-- swapchain). Compiles the graph core, the Vulkan backend and the engine's own device judgement
-- (DeviceCaps + DeviceCapsProbe) directly, with vk-bootstrap and VMA - no engine singleton - and runs
-- clear -> sample -> compute -> copy under the validation layer with synchronization validation on.
-- Needs a Vulkan device and the LunarG validation layer (VULKAN_SDK).
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGBuilder.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGCompile.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanRenderGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/API/Vulkan/DeviceCaps.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/API/Vulkan/DeviceCapsProbe.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/vk-bootstrap/VkBootstrap.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/VulkanAllocator/vk_mem_alloc.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    externalincludedirs {
        deps.DesertSpecific.IncludeDir.base,
        deps.DesertSpecific.IncludeDir.Vulkan,
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
        links { deps.DesertSpecific.Libraries.Debug }

    filter "configurations:Release"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end
        links { deps.DesertSpecific.Libraries.Release }

    filter {}

print("Configured test project: " .. test_name)
