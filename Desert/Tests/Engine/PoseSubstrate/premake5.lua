local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- Units under test (pure CPU: no Vulkan symbols are referenced, only declaration-only headers).
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
        -- Animator.cpp runs the Controls stage, so it needs the control base it calls through. The base is
        -- two functions and no Vulkan; no suite here adds a control, which is what makes "a rig with no
        -- controls behaves exactly as before" a thing these suites stillmeasure.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        -- Animator.cpp also runs the Rig stage (T5.4), which is a link edge and not a behaviour these
        -- suites exercise: none of them attaches a rig, which is what keeps "a pipeline with no rig
        -- produces exactly what it produced before" measurable HERE rather than only in the rig suite.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        -- T5.5: the stage owns a forwards solve, so the walk links with the stage that runs it.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
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
