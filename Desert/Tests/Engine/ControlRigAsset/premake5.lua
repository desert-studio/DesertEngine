local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The format under test, and the runtime it converts to and from. The stage, the hierarchy and the
        -- Animator are here because the load-bearing assertion of this suite is not "the bytes round-trip"
        -- — it is "a rig reaches the SKINNING MATRICES", which needs the whole pose pipeline compiled.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/ControlRig.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        -- T5.5: the stage owns a forwards solve, so the walk links with the stage that runs it.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Animator.cpp",
        -- The section blend the Animator samples through (AnimationClip::SampleTrack). A header-inline
        -- call into a .cpp nobody linked is a LINK error and not a silent wrong answer, which is why
        -- ApplySection lives in a translation unit rather than in the header beside its caller.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/ClipSection.cpp",
        -- A25: `Animator` owns an optional retarget, so every suite that compiles Animator.cpp links the
        -- retarget layer with it. Listed here rather than discovered at link time because premake
        -- enumerates sources EXPLICITLY: a dependency that is real but unlisted fails as an undefined
        -- symbol naming a function sitting in the working tree, which reads as a defect in the merge.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetSource.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }

    -- LINKED, not compiled in: SaveControlRigFile goes through Common's atomic write primitive, so the
    -- file half of the format is exercised by the same call the editor makes rather than by a local
    -- ofstream this suite invents. Common carries Objective-C (the macOS file dialog), which is why the
    -- two frameworks come with it — FileSystemWrite's premake makes the same pair for the same reason.
    links { "Common", "Optick" }

    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

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
