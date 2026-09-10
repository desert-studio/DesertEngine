local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Both units under test are header-only and reach nothing but Common's write primitive, rfl::json and
    -- std::filesystem -- which is the point. WriteCookedJson was two identical copies inside two importer
    -- .cpp files and the Blender script writer was a private static of BlendImporter (which includes
    -- Assimp); in both shapes the one branch worth asserting about them could not be compiled here at all.
    files {
        test_files,
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Editor/Source", -- <Editor/Import/CookedJsonWrite.hpp>
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the cooked metadata IS rfl::json
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

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

    -- Common contains Objective-C (the MacOS file dialog) and this suite reaches
    -- Common::Utils::FileSystem, so the ObjC runtime + AppKit link too.
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
