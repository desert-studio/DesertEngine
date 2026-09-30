-- SceneMigrator — the ONLY thing in this repository that knows an old .desce or .deprefab format.
--
-- The migrations used to live in the engine (Engine/Core/Serialize/SceneMigration.cpp) and run on every
-- scene load; they are Source/SceneMigration.cpp here now, and the engine loader REFUSES an old file and
-- names this tool instead. Nothing about the functions changed — they were always pure functions over the
-- parsed tree, needing no GPU, no asset manager and no scene graph, which is exactly what let them move.
--
-- It still reaches into the engine for HEADERS and not for code: the current on-disk struct
-- (Engine/Core/Serialize/SceneFormat.hpp), the tonemap enum and the shipped cloud-type names. Those are
-- statements of the CURRENT format, and a second copy of any of them here is a format that can fork.
-- dofile, not include: the dependency list was already include()'d by the engine projects.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "SceneMigrator"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        -- THE ENGINE'S OWN REFLECTION TABLE, not a copy of it. Source/SettingsCanonical.cpp writes the
        -- Settings block the way the engine's saver writes it, which means enumerating the same 51 fields
        -- in the same order through the same serializer. A hand-written field list here would be a second
        -- statement of the format, which is the fork this tool's own header forbids. It is kept OUT of
        -- SceneMigration.cpp on purpose: sixteen suites compile that file to test one schema step each,
        -- and none of them should have to link an engine reflection table to do it.
        --
        -- It costs nothing but compile time: these three compile against Common alone, with no GPU, no
        -- window and no Desert link (Desert/Tests/Engine/ConfigOwnership and SceneForeignKeys build on
        -- exactly this recipe). Reflection.gen.cpp is emitted by DesertHeaderTool as a PREBUILD STEP OF
        -- `Desert`, hence the dependency below.
        -- THE ANIM GRAPH'S JSON ROUND TRIP, for the v20 -> v21 step. 24 lines of reflect-cpp over plain
        -- structs, with no Desert link behind it: the blob that step moves out of the entity IS this type
        -- serialized, so reading it with a hand-written parser here would be a second statement of the
        -- format — the fork this tool's own header forbids.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        -- THE FOLIAGE TYPE FORMAT, for the v32 -> v33 step: the `.defoliage` it writes is the engine's own
        -- WriteFoliageType output, not a second statement of the format.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/FoliageType.cpp",
        -- THE EDITMESH AND ITS SAVED FORM, for the v21 -> v22 step: the step welds the v21 render arrays with
        -- the editor's own Geometry::FromRenderMesh and writes Geometry::ToSerialized, so the block it stores
        -- is the one the loader reads - not a second statement of either format.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp", -- the v22 -> v23 step bakes tiles
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",

        -- THE PREFAB GATE AND THE ONE WRITER OF .deprefab TEXT, since И11. The tool converts prefabs
        -- too, and the bytes it writes must be the bytes the engine's saver produces and must pass the
        -- engine's own loader gate — so it calls WritePrefabJson / ParseLoadablePrefab rather than
        -- restating either. Pure over the parsed tree, like everything else in this project: no GPU, no
        -- asset manager.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Serialize/ExternalEntities.cpp", -- v35 reads and writes one file per entity (WP16)

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
        -- SKEL-TREE: ANIM 4 -> 5 and MeshBinary 3/4 -> 5 resolve the bone hash to the one .skeleton with it
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/SkeletonReference.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        -- SKEL-fixa: a skinned import's record states its source's hash (Source/ImportRecordSourceHash) through
        -- the engine's own record reader/writer and the one source hash (HashMeshSourceFile); Common only.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/ImportRecord.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp",
    }

    dependson { "Desert" }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/Shared",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include",         -- PrefabData reaches ECS headers
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",  -- the scene tree is rfl::Generic
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",        -- LandscapeData.cpp compiles LandscapeHeight.glslh as C++
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    -- Common: UUID/AssetHandle/the logger the rejection warnings go through.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick", "ReflectCpp" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        -- Common contains Objective-C (file dialog); linking it needs AppKit + the ObjC runtime.
        links { "Cocoa.framework", "Foundation.framework" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}
