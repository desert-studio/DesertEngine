-- FDYNAMICMESH3 AS A .stmesh AND BACK (P7): the new core writes the same DESTMESH v2 bytes as the EditMesh
-- path for the generator shapes and every tracked scene mesh, reads its own file back to the same bytes, and
-- keeps the polygroups and the material slots across the file.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")
local scenes_dir = path.getabsolute(_SCRIPT_DIR .. "/../../../../Editor/Resources/Assets/Scenes")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
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
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshAssetArrays.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        -- the old core, the reference the byte equivalence is measured against
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAsset.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }

    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- PrefabData reaches the asset headers
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the block is written and read as JSON text
    }

    -- the tracked scene corpus the equivalence runs over
    defines { 'DESERT_SCENES_DIR="' .. scenes_dir .. '"' }

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
