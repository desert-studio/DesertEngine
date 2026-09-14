local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit under test is the CONSTRUCTOR of Engine/Assets/CloudNoiseVolumeAsset.cpp — where the
    -- asset's identity is derived from its path. The container source next to it is listed because the
    -- asset's Load calls into it and the linker wants the symbol, not because the format is under test
    -- here; that is the CloudNoiseVolume suite's job.
    --
    -- The engine sources are listed rather than linked because libDesert pulls in Vulkan and the whole
    -- renderer; neither is needed to construct an asset and read its handle, which is exactly why the
    -- asset layer was kept free of GPU types.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
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

    -- Common: the VFS the asset reads through, the filesystem helper, the logger and the Result type.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- BROKEN BY Д35, NOT BY WHAT THIS SUITE TESTS, and it is a LINK failure so nothing here changed
    -- shape. `CloudNoiseVolumeAsset::Save` moved onto Common::Utils::FileSystem::WriteBytesToFileAtomic,
    -- which pulls FileSystem.o out of libCommon.a — and that object references the Objective-C file
    -- dialogs in MacOSFileSystem. Every suite that already touched FileSystem carries these two
    -- frameworks for exactly this reason (Desert/Tests/Common/FileSystemWrite and four neighbours);
    -- this one had never needed them until the asset's writer moved.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
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
