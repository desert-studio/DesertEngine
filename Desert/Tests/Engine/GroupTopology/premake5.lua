local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshCore/SmallListSet.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3_Queries.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3_Edits.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshOverlay.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshCore/DynamicMesh/GroupTopology.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshCore/MeshRegionBoundaryLoops.cpp",
        -- the ported-core element selection (P10) and, until P8b, the EditMesh path it shares its algorithms with
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/DynamicMeshSelection.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshSelection.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshNormals.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshPolyGroups.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",        -- <Engine/Geometry/MeshCore/*.hpp>
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
