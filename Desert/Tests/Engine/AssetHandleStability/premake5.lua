local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit under test is the CONSTRUCTOR of every asset class -- where an asset's identity is derived
    -- from its path. Each concrete type's translation unit is listed, plus the container sources their
    -- Load calls link against; the formats themselves are other suites' business.
    --
    -- The engine sources are listed rather than linked because libDesert pulls in Vulkan and the whole
    -- renderer, and neither is needed to construct an asset and read its handle. That the asset layer
    -- still compiles free of GPU types is itself worth keeping true.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/StaticMeshAsset.cpp",
        -- StaticMeshAsset loads its render form through the mesh DDC (AF4d), which reads the source asset.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp", -- TextureAsset reads the cooked container through it
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp", -- TextureBinary's BlockCompressChain encodes through it
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/SkinnedMeshAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/SkeletonAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/AnimationAsset.cpp",
        -- AnimationAsset::Load delegates the channel-list -> clip step to this pure unit.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Mesh/SurfaceMaterialAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/TextureAsset.cpp",
        -- TextureAsset reads the cooked texture key through it.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp",
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
        -- The UI theme: the asset wrapper and the format it parses. Neither reaches the GPU — a theme is a
        -- table of numbers and its one device-bound referent (a font atlas) is bound by the service, a
        -- layer up — which is why both compile straight into a suite that links no renderer.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/UIThemeAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/LandscapeLayerInfoAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/LandscapeLayerInfo.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/UIThemeData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/StringTableAsset.cpp",
        -- The shader graph: the asset wrapper and the FORMAT it parses. The node catalogue and the GLSL
        -- emitter are NOT here and cannot be -- they live in Editor/ -- which is the seam the format was
        -- split along in the first place: this suite constructs the asset and reads its handle, and the
        -- handle is a function of the path, not of what a node means.
        -- The anim graph: the asset wrapper and the JSON round trip its Load calls. Plain structs over
        -- reflect-cpp, no GPU, no renderer — the same recipe as the rig two blocks up.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/AnimGraphAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/ShaderGraphAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/ShaderGraph.cpp",
        -- The control rig: the asset wrapper and the format it parses, plus the two Animation units the
        -- format converts to and from. None of them reaches the GPU — a rig is names and transforms —
        -- which is why they compile straight into a suite that links no renderer.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/ControlRigAsset.cpp",
        -- A25: the catalogue instantiates every asset type, so a new one is linked here or the
        -- census cannot ask its question about it.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/RetargetAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/Retarget.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/FoliageTypeAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/FoliageType.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/ControlRig.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        -- T5.5: the stage owns a forwards solve, so the walk links with the stage that runs it.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/StringTable.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocaleFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocalizedText.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocalizationService.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/PluralRules.cpp",
        -- SkeletonAsset::Load builds an Animation::Skeleton, whose constructor computes the signature.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
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
