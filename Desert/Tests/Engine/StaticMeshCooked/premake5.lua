local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit under test is the COOKED STATIC MESH LOADER, so StaticMeshAsset.cpp is listed as a source
    -- rather than linked: libDesert pulls in Vulkan and the whole renderer, and none of it is needed to
    -- read a .stmesh off disk -- that the asset layer still compiles free of GPU types is itself worth
    -- keeping true. Same shape as Desert/Tests/Engine/SkinnedMeshDependency.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/StaticMeshAsset.cpp",
        -- StaticMeshAsset loads its render form through the mesh DDC (AF4d), which reads the source asset.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        -- The editor's mesh builder derives the probe's render form on a DDC miss (list as in MeshDerivedData).
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshAssetArrays.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshNormals.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/EditMeshXformOperations.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshLOD.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Geometry/MeshSimplifier.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/MeshDeriver.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/SourceToEngine.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
    }
    externalincludedirs {
        deps.DesertSpecific.IncludeDir.meshoptimizer,
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",
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

    -- Common: the filesystem helper the loader reads through, the logger, the Result type and UUID.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick", "MeshOptimizer" }

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
