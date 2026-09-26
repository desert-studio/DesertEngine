local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/SmallListSet.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/MeshIndexUtil.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3_Queries.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3_Edits.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshOverlay.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/MeshRegionBoundaryLoops.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/MeshBoundaryLoops.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/Selections/MeshConnectedComponents.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/Selections/QuadGridPatch.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/Operations/EmbedSurfacePath.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/GroupEdgeInserter.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/SimpleHoleFiller.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMeshEditor.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/OffsetMeshRegion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/InsetMeshRegion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/PolyEditingEdgeUtil.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/CompGeom/PolygonTriangulation.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/MeshBevel.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/PolyEditingUVUtil.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Parameterization/DynamicMeshUVEditor.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/Solvers/MeshUVSolver.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/MeshTangents.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/GroupTopology.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/SplitAttributeWelder.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/Operations/MergeCoincidentMeshEdges.cpp",
        -- RunRegionOperation and what it and the tangent-cube test call: selection, render conversion
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshRegionOperation.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/DynamicMeshSelection.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/DynamicMeshRenderConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshSelection.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshNormals.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshPolyGroups.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",        -- <Engine/Geometry/MeshCore/*.hpp>
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

    links { "Common", "Optick" }

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
