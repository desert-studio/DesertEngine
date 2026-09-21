local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The subject is the SHIPPED IK rig, mesh, clip and the two scenes under Editor/, read through the
    -- engine's own reader — plus, unlike TwoBoneWitness next door, the ANIMATOR AND THE CONTROL. That is a
    -- deliberate difference: TwoBoneWitness asserts a property of its rig and must keep meaning that while
    -- the substrate is rewritten, whereas the question here is whether the shipped scene's authored goal is
    -- actually reached by the shipped solver from the shipped bytes. Nothing in this list pulls in Vulkan.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Animator.cpp",
        -- A25: `Animator` owns an optional retarget, so every suite that compiles Animator.cpp links the
        -- retarget layer with it. Listed here rather than discovered at link time because premake
        -- enumerates sources EXPLICITLY: a dependency that is real but unlisted fails as an undefined
        -- symbol naming a function sitting in the working tree, which reads as a defect in the merge.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetSource.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        -- Animator.cpp also runs the Rig stage (T5.4), which is a link edge and not a behaviour these
        -- suites exercise: none of them attaches a rig, which is what keeps "a pipeline with no rig
        -- produces exactly what it produced before" measurable HERE rather than only in the rig suite.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        -- T5.5: the stage owns a forwards solve, so the walk links with the stage that runs it.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/ClipSkeletonMatch.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        -- The tick grid every clip time now lives on (A5).
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TwoBoneIKControl.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{wks.location}/Desert/Common/Source/Common/Core/Timestep.cpp",
        -- The cooked-mesh container the fixtures are written in (B11).
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
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
