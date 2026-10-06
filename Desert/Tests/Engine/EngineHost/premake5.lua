-- EngineHost: the ENGINE itself, headless, on a real device. Unlike RenderGraphVulkan (which compiles the graph
-- sources against its own vk-bootstrap device) this suite links Desert.lib and boots the engine as Application
-- does, minus the window: VulkanContext (instance), Device::Create (VulkanLogicalDevice), RendererContext::Init
-- (VMA + CommandBufferAllocator), Renderer::Init. A frame with no window records into the graph's own
-- command buffer and PresentFinalImage submits and waits (VulkanRendererAPI::SubmitHeadlessFrame), so a test
-- reads back what Renderer::DispatchCompute wrote, byte for byte.
-- Needs a Vulkan device (VULKAN_SDK for the validation layer in Debug).
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')
local test_name = path.getname(_SCRIPT_DIR)

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files { os.matchfiles("*.cpp") }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    externalincludedirs {
        deps.DesertSpecific.IncludeDir.base,
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/GLFW/include/",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/",
    }
    for _, key in ipairs({ "Vulkan", "shaderc", "spirv_cross" }) do
        local p = deps.DesertSpecific.IncludeDir[key]
        if p then externalincludedirs { p } end
    end
    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end
    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end
    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end
    -- The same Optick switches the Editor builds against Desert.lib with.
    defines { "USE_OPTICK=1", "OPTICK_ENABLE_GPU=0", "OPTICK_ENABLE_TRACING=0" }

    -- What the Editor links next to Desert.lib (Editor/premake5.lua), minus the editor's own toolkit use.
    links { "Desert", "GLFW", "Optick", "MeshOptimizer", "OpenSubdiv", "ImGui", "Assimp", "OpenEXRCore", "Dav1d", "Opus" }

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
        buildoptions { "/bigobj" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        -- gmake links no static-lib dependency transitively: what Desert.lib uses and the frameworks GLFW/MoltenVK
        -- use, as the Editor lists them.
        links { "Common", "Jolt", "Lua", "ReflectCpp",
                "Cocoa.framework", "IOKit.framework", "CoreFoundation.framework", "CoreVideo.framework",
                "CoreMedia.framework", "AVFoundation.framework", "QuartzCore.framework", "Foundation.framework" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        for name, path in pairs(deps.TestSpecific.Libraries.Debug) do
            links { path }
        end
        links { deps.DesertSpecific.Libraries.Debug }

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end
        links { deps.DesertSpecific.Libraries.Release }

    filter {}

print("Configured test project: " .. test_name)
