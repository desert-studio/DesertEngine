local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The world cook (Engine/Core/Serialize/WorldCells.cpp) and the tool that drives it, compiled as they
    -- ship: the suite runs Tools/WorldCook's own RunWorldCook, not a re-implementation of it.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/WorldCells.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/WorldCellLoader.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/SceneFormat.cpp",
        "%{wks.location}/Tools/WorldCook/Source/WorldCookMain.cpp",
        -- The planner places a landscape tile by its root's frame; both files are pure and link only Common.
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeLayout.cpp",
    }

    includedirs {
        -- LandscapeData.cpp compiles Shaders/Common/LandscapeHeight.glslh as C++.
        "%{wks.location}/Editor/Resources/Shaders",
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Tools/WorldCook/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the record's component payloads are rfl::Generic
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

    -- PrefabData.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs to know the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        -- Common contains Objective-C (file dialog), and the tool this suite runs writes through Common's
        -- file primitives.
        links { "Cocoa.framework", "Foundation.framework" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: UUID and AssetHandle. Optick: Common's JobSystem registers its worker threads with it.
    links { "Common", "Optick" }

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
