local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The subject is the SHIPPED corpus files under Editor/Cooked/Meshes, read through the engine's own
    -- reader: the clip build step (the pure function AnimationAsset::Load calls), the skeleton whose
    -- constructor derives the signature the clips are matched against, and the match rule itself. The
    -- engine sources are listed rather than linked because libDesert pulls in Vulkan and the whole
    -- renderer, and none of it is needed to read a clip off disk -- that the animation layer still
    -- compiles free of GPU types is itself worth keeping true. Same shape as SkinnedMeshDependency.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/ClipSkeletonMatch.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        -- The tick grid every clip time now lives on (A5).
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
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

    -- Common: the Result type, the logger and UUID. Optick: Common's JobSystem registers its worker
    -- threads with the profiler.
    links { "Common", "Optick" }

    -- Common contains Objective-C (MacOSFileSystem's file dialog), so the ObjC runtime + AppKit link too.
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
