local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit under test is the MISSING-FILE branch of every loader this list can compile - the same
    -- GPU-free translation-unit list as AssetHandleStability, because the loaders are the same files.
    -- See asset_missing_file_test.cpp for why these branches were dead until 2026-09-05.
    --
    -- The engine sources are listed rather than linked because libDesert pulls in Vulkan and the whole
    -- renderer, and neither is needed to construct an asset and read its handle. That the asset layer
    -- still compiles free of GPU types is itself worth keeping true.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/StaticMeshAsset.cpp",
        -- StaticMeshAsset loads its render form through the mesh DDC (AF4d), which reads the source asset.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/SkinnedMeshAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/SkeletonAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/AnimationAsset.cpp",
        -- AnimationAsset::Load delegates the channel-list -> clip step to this pure unit.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/SurfaceMaterialAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/TextureAsset.cpp",
        -- TextureAsset reads the cooked texture key through it.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp",
        -- TextureAsset reads the cooked container through it (B17).
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp", -- TextureBinary's BlockCompressChain encodes through it
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Skybox/SkyboxAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Shader/ShaderAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudTypeAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolumeAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayoutAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        -- SkeletonAsset::Load builds an Animation::Skeleton, whose constructor computes the signature.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",
        "%{wks.location}/ThirdParty/reflect-cpp/include",
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

    -- Common: the VFS the assets read through, the filesystem helper, the logger, the Result type and
    -- UUID itself. Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- Common contains Objective-C (MacOSFileSystem's file dialog) and the asset loaders reach
    -- Common::Utils::FileSystem, so the ObjC runtime + AppKit have to link as well.
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
