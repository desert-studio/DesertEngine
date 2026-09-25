local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Two units under test, and the point of the suite is the RELATION between them:
    --   * Engine/Assets/CloudNoiseVolume.cpp — the .dcnv container: encode, decode, and every refusal;
    --   * Engine/Assets/CloudNoiseVolumeGenerator.cpp — the offline bake, which itself compiles
    --     Editor/Resources/Shaders/Common/CloudNoise.glslh AS C++.
    -- The test compiles that same .glslh a second time (through CloudNoiseVolumeReference.hpp) so it can
    -- ask "what should voxel (x,y,z) hold" and compare it against what the generator actually wrote. That
    -- is why the SHADER ROOT is on the include path.
    --
    -- The engine sources are listed rather than linked because libDesert pulls in Vulkan and the whole
    -- renderer; these two translation units need neither, which is exactly why the volume format was put
    -- where it could be tested without a device.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeGenerator.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Resources/Shaders",
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

    -- Common: CloudNoiseVolume.cpp writes and reads the AF1 asset envelope (AssetGuid, WriteAssetEnvelope,
    -- ReadAssetEnvelope) that lives in Common::Content.
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
