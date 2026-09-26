local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- AF4c: the mesh deriver's key + DDC load (Engine) and the editor's builder, with the geometry it runs.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshAssetArrays.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshNormals.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshXformOperations.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshLOD.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/MeshSimplifier.cpp",
        "%{wks.location}/Editor/Source/Editor/Import/MeshDeriver.cpp",
        "%{wks.location}/Editor/Source/Editor/Import/ImportedMeshAsset.cpp",
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

    links { "Common", "Optick", "MeshOptimizer" } -- Commons JobSystem registers worker threads with Optick

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
