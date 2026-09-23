-- A landscape's root, its 2 x 2 tiles and the tile files beside the scene: saved, loaded and saved again
-- to the same bytes. Engine headers are compiled for the component blocks (AuthoredComponentIO.hpp), which
-- is why entt is on the include path; nothing of the renderer is linked.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The tile's heights and the tiling: both pure, linking only Common. SceneSerializer.cpp, which
        -- calls them on a real save, reaches the renderer through Scene.hpp and no suite compiles it —
        -- so the trip is asserted on the pieces it is made of, through the text and bytes on disk.
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeTileFiles.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- the component headers are ECS headers
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the block is written and read as JSON text
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

    -- Common: a UUID is Common::UUID and the IO header logs its refusals through Common's logger.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

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
