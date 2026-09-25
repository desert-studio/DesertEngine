local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- CookPaths.hpp IS the subject and it is header-only, so the test compiles the importer's real
    -- formula rather than a restatement of it. Nothing else from the importer is pulled in: the assimp
    -- side of AssimpImporter.cpp would drag the whole parser into a unit test for a string.
    files {
        test_files,
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Editor/Source",  -- <Editor/Import/CookPaths.hpp>
        "%{wks.location}/Desert/Desert/Source", -- CookPaths forwards to <Engine/Assets/CookedTexturePath.hpp>
        "%{wks.location}/Editor/Source/Editor/Import",
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

    -- Common: the content-root constants CookPaths reads, and AssetHandle::FromKey, which is what turns
    -- the key this suite is about into the id a .demat carries. Optick: Common's JobSystem registers its
    -- worker threads with the profiler.
    links { "Common", "Optick" }
    -- MaterialAdoption reads the .demat through Common's FileSystem, whose macOS half is Objective-C.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    filter "system:not windows"
        links { "ReflectCpp" }
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
