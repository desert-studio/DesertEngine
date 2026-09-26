local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The writer is header-only (Editor/Import/StaticMeshOutput.hpp); the conversion and the binary format
    -- are compiled in, so the relation EditMesh -> .stmesh -> EditMesh runs with no editor and no GPU.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshAssetArrays.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
        -- The ported core (P8a): the writer takes an FDynamicMesh3, built from the suite's EditMesh fixtures
        -- through the saved form.
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/SmallListSet.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3_Queries.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3_Edits.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshOverlay.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/Polygroups/PolygroupSet.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/DynamicMeshRenderConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/DynamicMeshSerialization.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/DynamicMeshAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
        -- The viewer's statistics (AV1f) are read from the probe's BUILT platform data, so the editor's mesh
        -- builder and what it compiles in are part of this suite (list as in StaticMeshCooked).
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshNormals.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshXformOperations.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshLOD.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshSimplifier.cpp",
        "%{wks.location}/Editor/Source/Editor/Import/MeshDeriver.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/VoxelBlockout.cpp", -- CG1: the blockout Accept writes
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
    }
    externalincludedirs {
        deps.DesertSpecific.IncludeDir.meshoptimizer,
        "%{wks.location}/ThirdParty/reflect-cpp/include",
        "%{wks.location}/ThirdParty/entt/include/",
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

    links { "Common", "Optick", "MeshOptimizer" } -- Optick: Commons JobSystem; MeshOptimizer: the LOD builder

    filter "system:not windows"
        links { "ReflectCpp" }

    -- Common contains Objective-C (the MacOS file dialog), so the ObjC runtime + AppKit link too.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }

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
