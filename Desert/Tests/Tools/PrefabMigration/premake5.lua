local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool as a PREBUILD STEP OF `Desert`. Without this edge
    -- a parallel build can compile a stale table in and the assertions would report on yesterday's struct.
    dependson { "Desert" }

    -- The SHARED step chain (SceneMigration.cpp — the prefab entry point lives there, beside the scene
    -- one, because they are the same steps) and the engine gate the migration must agree with. Nothing
    -- else: both are pure functions over the parsed tree, and this project linking without a renderer,
    -- an asset manager or a scene is the proof, exactly as it is for the scene migration suites beside it.
    files {
        test_files,
        "%{wks.location}/Tools/SceneMigrator/Source/SceneMigration.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp", -- the v22 -> v23 step bakes tiles
        -- The anim graph's JSON round trip: schema step 21 moves the state machine out of the entity and
        -- reads it with the engine's own parser, so every suite that compiles the migration links it too.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        -- The migration lives in the TOOL (the engine loader only refuses; see PrefabFormat.hpp). This
        -- is what makes `#include <SceneMigration.hpp>` resolve.
        "%{wks.location}/Tools/SceneMigrator/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- PrefabData reaches ECS headers
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the prefab tree is rfl-serialized
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

    -- PrefabData.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
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
