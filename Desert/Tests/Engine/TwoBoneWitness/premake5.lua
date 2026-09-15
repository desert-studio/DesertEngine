local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The subject is the SHIPPED two-bone rig, mesh and clips under Editor/Cooked/Meshes, read through the
    -- engine's own reader, plus the one-bone corpus next door which this suite uses as its CONTROL. Listed
    -- rather than linked for the same reason as AnimationClipCorpus: libDesert pulls in Vulkan and the whole
    -- renderer, none of which is needed to read a rig off disk, and keeping the animation layer compilable
    -- free of GPU types is worth something on its own.
    --
    -- Animator.cpp is deliberately NOT here. What this suite asserts is a property of the RIG — that the two
    -- candidate blend orders are separable on it — and it has to keep meaning that while the pose substrate
    -- underneath is rewritten.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/ClipSkeletonMatch.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        -- The tick grid every clip time now lives on (A5).
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        -- Pose.cpp arrived with the merge of А1, which moved BoneTransform's P/R/S <-> mat4 conversion
        -- out of the header. This suite composes a bone's matrix itself, so it needs the definition of
        -- BoneTransform::ToMatrix; without this line the suite COMPILES and fails at LINK, naming a
        -- symbol whose source is sitting in the tree. Adding it does not weaken the note above —
        -- Animator.cpp is still absent, so what is asserted is still a property of the rig.
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
