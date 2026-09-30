local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Unlike the suites beside it, this one compiles the TOOL and not only the migrations: the claim
    -- under test is the file loop itself (read, write back atomically, exit code), which lives in
    -- MigratorMain.cpp behind RunSceneMigrator. SceneMigration.cpp comes along because the loop
    -- calls MigrateScene on every file it parses.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/MigratorMain.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Serialize/ExternalEntities.cpp", -- a partitioned world is read joined (WP16)
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/SceneMigration.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/SkeletonReference.cpp", -- SKEL-TREE raises (MSAS SRCE 3, MeshBinary 5, ANIM 5) in SceneMigration.cpp
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp", -- SKEL-TREE raises (MSAS SRCE 3, MeshBinary 5, ANIM 5) in SceneMigration.cpp
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp", -- SKEL-TREE raises (MSAS SRCE 3, MeshBinary 5, ANIM 5) in SceneMigration.cpp
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/FoliageType.cpp", -- the v32 -> v33 step
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp", -- the v22 -> v23 step bakes tiles
        -- The anim graph's JSON round trip: schema step 21 moves the state machine out of the entity and
        -- reads it with the engine's own parser, so every suite that compiles the migration links it too.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
        -- The tool's loop canonicalises the Settings block through the ENGINE'S reflection table, so a
        -- suite that compiles MigratorMain.cpp has to bring the table with it. It is deliberately not in
        -- SceneMigration.cpp: the fifteen suites that test one schema step each must stay free of it.
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/SettingsCanonical.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        -- The loop raises `.deprefab` files too (И11), and the bytes it writes for one come from the
        -- ENGINE'S own writer, gate-checked by the ENGINE'S own loader gate — so a suite that compiles
        -- the loop has to bring that pair with it, for the same reason it brings the reflection table.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp",
        -- THE CLOUD NOISE VOLUME DECODER, since T7g: the DCNV 1/2 -> 3 step wraps the payload in the AF1
        -- envelope and reads it back through the engine's own DecodeCloudNoiseVolume before writing. Pure
        -- bytes in, bytes out; no GPU.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
        -- THE SCULPTED CLOUD VOLUME DECODER, likewise for the DCMV 2 -> 3 step (DecodeCloudModellingVolume).
        -- It reaches Common's JobSystem and Rounding, both inside the Common this project already links.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
        -- THE MESH ASSET READER, since MIG1: a `.stmesh`/`.skmesh` stamped 'MSAS' (AF4d) is judged by the
        -- engine's own DecodeMeshSourceAsset rather than refused as "not DESTMESH"; pure bytes, no GPU.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
        -- SKEL-fixa: the loop states a skinned import's source hash and box in its record (ImportRecordSourceHash)
        -- through the engine's own record reader/writer and the one source hash (HashMeshSourceFile).
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ImportRecordSourceHash.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/ImportRecord.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp",
    }

    -- Reflection.gen.cpp is emitted by DesertHeaderTool as a prebuild step of `Desert`.
    dependson { "Desert" }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        -- The migrations and the tool's loop live in the TOOL (see SceneMigratorEndToEnd's premake
        -- for why they moved out of the engine). This resolves <MigratorMain.hpp> and
        -- <SceneMigration.hpp>, and their own includes of themselves.
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",       -- Components.hpp is an entt registry away
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

    -- Components.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: UUID/AssetHandle/the logger + the atomic write primitive the tool's loop goes through.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- Common contains Objective-C (MacOSFileSystem file dialog) — referencing FileSystem pulls it in,
    -- so the ObjC runtime + AppKit must link too.
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
