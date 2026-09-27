local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- TWO units under test, and the point of the suite is the relation BETWEEN them, GPU-free:
    --   * Editor/Resources/Shaders/Common/CloudShadowMap.glslh — the triple's encode, its reconstruction
    --     and the march that fills it — compiled AS C++ through CloudShadowReference.hpp, together with
    --     Common/CloudGeometry.glslh for the shell the ray crosses. That is why the SHADER ROOT is on the
    --     include path: the test drives the exact text the two cloud passes compile.
    --   * Engine/Graphic/Clouds/CloudShadowPayload.hpp — the C++ mirror of those constants and the
    --     orthographic projection with its two snaps. Header-only and pure, so nothing is linked from the
    --     engine for it.
    -- Nothing to link — no renderer, no Vulkan.
    files {
        test_files,
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        -- CloudShadowPayload.hpp -> CloudPayload.hpp -> VolumetricCloudComponent.hpp, which reaches the
        -- reflection macros and, through Assets/Common.hpp, an entt registry.
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",
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
