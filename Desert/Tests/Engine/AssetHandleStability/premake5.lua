local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit under test is the CONSTRUCTOR of every asset class -- where an asset's identity is derived
    -- from its path. Each concrete type's translation unit is listed, plus the container sources their
    -- Load calls link against; the formats themselves are other suites' business.
    --
    -- The engine sources are listed rather than linked because libDesert pulls in Vulkan and the whole
    -- renderer, and neither is needed to construct an asset and read its handle. That the asset layer
    -- still compiles free of GPU types is itself worth keeping true.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/StaticMeshAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/SkinnedMeshAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/SkeletonAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/AnimationAsset.cpp",
        -- AnimationAsset::Load delegates the channel-list -> clip step to this pure unit.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/SurfaceMaterialAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/TextureAsset.cpp",
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
        -- The UI theme: the asset wrapper and the format it parses. Neither reaches the GPU — a theme is a
        -- table of numbers and its one device-bound referent (a font atlas) is bound by the service, a
        -- layer up — which is why both compile straight into a suite that links no renderer.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/UIThemeAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/UIThemeData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/StringTableAsset.cpp",
        -- The shader graph: the asset wrapper and the FORMAT it parses. The node catalogue and the GLSL
        -- emitter are NOT here and cannot be -- they live in Editor/ -- which is the seam the format was
        -- split along in the first place: this suite constructs the asset and reads its handle, and the
        -- handle is a function of the path, not of what a node means.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/ShaderGraphAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/ShaderGraph.cpp",
        -- The control rig: the asset wrapper and the format it parses, plus the two Animation units the
        -- format converts to and from. None of them reaches the GPU — a rig is names and transforms —
        -- which is why they compile straight into a suite that links no renderer.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/ControlRigAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/ControlRig.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/StringTable.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocaleFormat.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocalizedText.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocalizationService.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/PluralRules.cpp",
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
