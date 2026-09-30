local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- Units under test (pure CPU: no Vulkan symbols are referenced, only declaration-only headers).
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Animator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/LayeredBlendPerBone.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraphInstance.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraph.cpp",
        -- PoseGraphInstance builds the state machine's Evaluator (AnimGraphEvaluator.cpp).
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphEvaluator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphValidation.cpp",
        -- A clip IS its Timeline::Sequence (ANIM-I8b): the Animator binds its bone tracks, evaluates the pose,
        -- fires its Event keys and reads its Float tracks through the Timeline layer, so that layer links here.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Channel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Player.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Binding.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Track.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Sequence.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Evaluator.cpp",
        -- A25: `Animator` owns an optional retarget, so every suite that compiles Animator.cpp links the
        -- retarget layer with it. Listed here rather than discovered at link time because premake
        -- enumerates sources EXPLICITLY: a dependency that is real but unlisted fails as an undefined
        -- symbol naming a function sitting in the working tree, which reads as a defect in the merge.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetSource.cpp",
        -- Animator.cpp runs the Controls stage, so it needs the control base it calls through. The base is
        -- two functions and no Vulkan; no suite here adds a control, which is what makes "a rig with no
        -- controls behaves exactly as before" a thing these suites stillmeasure.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        -- Animator.cpp also runs the Rig stage (T5.4), which is a link edge and not a behaviour these
        -- suites exercise: none of them attaches a rig, which is what keeps "a pipeline with no rig
        -- produces exactly what it produced before" measurable HERE rather than only in the rig suite.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        -- T5.5: the stage owns a forwards solve, so the walk links with the stage that runs it.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        -- The tick grid every clip time now lives on (A5).
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
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

    -- A clip's bindings carry GUIDs (Timeline::BindingGuid::Generate -> Common's AssetGuid); Common's
    -- JobSystem needs Optick and its file dialog is Objective-C (the link TimelineContract states).
    links { "Common", "Optick" }
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
