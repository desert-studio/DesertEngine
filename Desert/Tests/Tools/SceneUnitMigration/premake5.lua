local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit migration is a pure function over the parsed tree, so this test needs exactly one engine
    -- translation unit and no renderer, scene or asset manager. If that ever stops being true, the
    -- migration has stopped being pure and the contract's §4.4 is broken before this file is.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/SceneMigration.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp", -- the v22 -> v23 step bakes tiles
        -- The anim graph's JSON round trip: schema step 21 moves the state machine out of the entity and
        -- reads it with the engine's own parser, so every suite that compiles the migration links it too.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        -- The migrations live in the TOOL now (they used to be an engine TU that ran on every
        -- scene load). This is what makes `#include <SceneMigration.hpp>` below resolve, and its
        -- own `#include "SceneMigration.hpp"` of itself.
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",       -- PrefabData reaches ECS headers
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the scene tree is rfl::Generic
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

    -- Common: UUID/AssetHandle/the logger the rejection warnings go through.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
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
