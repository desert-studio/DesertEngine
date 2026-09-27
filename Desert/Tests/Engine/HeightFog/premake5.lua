local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Two units under test, both GPU-free:
    --   * Editor/Resources/Shaders/Common/HeightFog.glslh — the closed-form exponential height-fog
    --     integral, the falloff Taylor branch, start/cutoff handling, the max-opacity clamp and the
    --     directional lobe — compiled AS C++ through HeightFogReference.hpp and pinned against a
    --     brute-force numeric integration of the same medium. That is why the SHADER ROOT is on the
    --     include path: the test drives the exact text the fog pass compiles.
    --   * Engine/Graphic/Fog/FogPayload.hpp — the component-to-GPU packing and its unit conversions.
    --     Header-only, so it links without the engine.
    -- Nothing to link — no renderer, no Vulkan.
    files {
        test_files,
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
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
