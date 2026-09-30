local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- THE SWEEP WITHOUT A DEVICE. AssetEviction.cpp is deliberately free of the GPU layer — the four
    -- calls that reach it live in AssetEvictionServices.cpp, behind IEvictionSink, and this suite passes
    -- a recording double instead. The same translation-unit list as AssetMissingFile plus the evictor,
    -- for the same reason: libDesert pulls in Vulkan and the whole renderer, and none of it is needed to
    -- decide which assets nothing reaches any more.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/AssetEviction.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp", -- TextureAsset reads the cooked container through it
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp", -- TextureBinary's BlockCompressChain encodes through it
        -- A25: the closure marks the retarget's SOURCE RIG, which is reachable through nothing else,
        -- so the sweep has to link the type it probes for. The format comes with it because the asset
        -- parses its own file.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/RetargetAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/Retarget.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/StaticMeshAsset.cpp",
        -- StaticMeshAsset loads its render form through the mesh DDC (AF4d), which reads the source asset.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/SkinnedMeshAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/SkeletonAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/AnimationAsset.cpp",
        -- AnimationAsset::Load delegates the channel-list -> clip step to this pure unit.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/SurfaceMaterialAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/TextureAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp", -- TextureAsset reads the asset's DDC key through it (AF7)
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Skybox/SkyboxAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/EnvironmentStaging.cpp", -- SkyboxAsset stages the environment cache on the worker
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Shader/ShaderAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/AssetRefSerialization.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudTypeAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudModellingVolumeAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudLayoutAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        -- THE ASYNC LOADER, because the sweep now ASKS it a question: AssetEviction refuses to release an
        -- asset whose read is in flight, and it finds that out from AsyncAssetLoader::IsRequested. Without
        -- this line the suite stopped linking the moment that refusal was added -- which is the suite
        -- doing its job, and the reason the refusal gets asserted here rather than only in the renderer.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/AsyncAssetLoader.cpp",
        -- AL1-6: the library indexes registry rows and reads clips through the loader.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/AnimationLibrary.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/ClipSkeletonMatch.cpp",
        -- SkeletonAsset::Load builds an Animation::Skeleton, whose constructor computes the signature.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        -- PrefabAsset is deliberately ABSENT. Its CreateFromEntity reaches ECS::Entity and the scene
        -- serializer, i.e. the whole world layer, and dragging that in to assert one Unload body would
        -- make this suite need a scene. Its contract is held textually instead, in
        -- Desert/Tests/Engine/AssetRoots, together with the other twelve.
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
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

    links { "Common", "Optick" }

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
