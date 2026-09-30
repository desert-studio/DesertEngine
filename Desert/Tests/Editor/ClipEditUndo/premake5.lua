-- "How many undo steps is one drag, and does the step put BOTH halves back."
--
-- The unit under test is Editor/Core/Commands/PoseEditTransaction.cpp, lifted out of SequencerPanel.cpp
-- and GizmoController.cpp for the reason scripts/CI/UnreachedSources.sh keeps naming: neither of those
-- two files is compiled by any suite, so a rule inside one cannot be checked and an edge missed there is
-- missed in silence. What is left in the panel is the two facts only it knows (which animator, which
-- clip, and whether the manipulator is held).
--
-- It links the REAL Animator and the REAL ControlKeyer rather than doubles of them, because the claim is
-- about a round trip through both: the keys the keyer writes on the release edge and the pose the panel
-- reloads from the clip afterwards are the state this transaction has to be able to put back, and a
-- double of either would be a second opinion about what that state is.
local test_name = path.getname(_SCRIPT_DIR)
-- notify_tracks_test.cpp tests AnimationNotifyTracks.hpp, which ANIM-I10d moves onto the Sequence; it rejoins then.
local test_files = os.matchfiles("*.cpp")
table.removevalue(test_files, "notify_tracks_test.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/Commands/PoseEditTransaction.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/Commands/SequenceEdit.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",

        -- The keying half of the round trip (same list as Desert/Tests/Engine/ControlKeying).
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlKeyer.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TrackEditing.cpp",

        -- RotateControlRecorded turns a control through ControlManipulator's RotateControlLocal, and the
        -- manipulator's translation unit also owns the shape frame, hence the shape library beside it.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlManipulator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlShape.cpp",

        -- The pose half. Animator.cpp is pure CPU (no Vulkan symbols), and every suite that compiles it
        -- links the retarget/rig/solver layer it owns -- enumerated because premake lists sources
        -- explicitly, so a real-but-unlisted edge fails as an undefined symbol naming a file that is
        -- sitting right there, which reads as a defect in the merge.
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
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetSource.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",

        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",       -- SequenceEdit.cpp reads UIAnimData (Components.hpp)
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
