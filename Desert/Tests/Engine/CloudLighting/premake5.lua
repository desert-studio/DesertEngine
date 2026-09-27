local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- One unit under test, GPU-free:
    --   * Editor/Resources/Shaders/Common/CloudLighting.glslh — the Henyey-Greenstein phase, Beer's law,
    --     the energy-conserving integral of one step and the profile-driven ambient occlusion — compiled
    --     AS C++ through CloudLightingReference.hpp. That is why the SHADER ROOT is on the include path:
    --     the test drives the exact text the cloud passes compile, so a passing test is a statement about
    --     the code the GPU runs.
    -- No renderer and no Vulkan. Common is linked since Р18 because the multiple-scattering reference
    -- reads the SHIPPED defaults out of ECS::VolumetricCloudData rather than restating them, and that
    -- header's AssetHandle members pull in Common::UUID. A restated copy of four floats is exactly the
    -- mirror-that-drifts this suite exists to prevent elsewhere.
    files {
        test_files,
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- reached through the cloud component's headers
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

    -- Common: Common::UUID, behind the AssetHandle members of the cloud component.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

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
