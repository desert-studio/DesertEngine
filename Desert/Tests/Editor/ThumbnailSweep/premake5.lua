-- "A thumbnail nobody asked for still arrives — and it never takes the machine with it."
--
-- ONE editor translation unit is compiled: ThumbnailScan.cpp, which is the half of the sweep that was
-- deliberately split away from ThumbnailService so that no Vulkan device stands between a test and the
-- decision. The other half (ThumbnailSweep.cpp, which calls the service) is not linkable here and is not
-- meant to be — what it does is drive the two things this suite proves.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Widgets/ThumbnailScan.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        -- CookPaths.hpp takes the cooked-texture path formula from Engine/Assets/CookedTexturePath.hpp
        -- (header-only: Common + std), so the engine's source root is on the path too.
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
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

    links { "Common", "Optick" } -- FileSystem/VFS live in Common; Common's JobSystem registers with Optick

    -- Common contains Objective-C (MacOSFileSystem's file dialog) — pulled in because this suite
    -- references FileSystem, so the ObjC runtime + AppKit must link too.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

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
