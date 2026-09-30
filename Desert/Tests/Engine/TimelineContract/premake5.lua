local test_name = path.getname(_SCRIPT_DIR)

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- ONE FILE PER CONTRACT GROUP: every group has landed (evaluator + format I4/I6, lift I7, layers
    -- I13/I14), so each lives in its own file below and nothing is stubbed.
    files {
        "timeline_main.cpp",
        "timeline_channel_test.cpp",
        "timeline_player_test.cpp",
        "timeline_section_test.cpp",
        "timeline_easing_test.cpp",
        "timeline_evaluator_test.cpp",
        "timeline_lift_test.cpp",
        "timeline_layered_test.cpp",
        "timeline_keying_test.cpp",
        "timeline_ui_lift_test.cpp",
        "TimelineFixtures.hpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Channel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Player.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Binding.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Track.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Sequence.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/SequenceFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Evaluator.cpp",
        -- generation 3 and its lift moved to the migrator (ANIM-I8a): the lift test builds the tool's own copy
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ClipLift.cpp",
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/ClipGeneration3.cpp",
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/UILift.cpp", -- the v40 -> v41 UI lift, the migrator's too (ANIM-I9)
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/LayeredBlendPerBone.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphEvaluator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphValidation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraphInstance.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        -- keying (timeline_keying_test.cpp): the sequence edits and the control keyer that writes through them
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TrackEditing.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlKeyer.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- SequenceFormat.cpp: the TMLN block IS rfl structs
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

    -- SequenceFormat.cpp writes through Common's JSON and canonical text; Common's JobSystem needs Optick and
    -- its file dialog is Objective-C (the same link AnimationClipFormat states).
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
