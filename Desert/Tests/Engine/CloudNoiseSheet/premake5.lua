local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The subject is the RELATION between two codecs that never see each other: the `.dcnv` container
    -- (CloudNoiseVolume.cpp) and the flat tiled slice sheet an artist's tool reads (CloudNoiseVolumeSheet.cpp).
    -- Import and export are only worth anything if a volume survives the trip through both, so the load-bearing
    -- assertion here is a BYTE EQUALITY across the pair rather than any property of either alone.
    --
    -- No generator and no shader root: this suite is about layout and refusal, not about noise maths. It fills
    -- volumes with a known pattern precisely so that a wrongly transposed slice is a visible index in the
    -- failure rather than a plausible-looking byte.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeSheet.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        includedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        includedirs { path }
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
