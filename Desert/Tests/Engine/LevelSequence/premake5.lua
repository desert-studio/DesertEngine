local test_name = path.getname(_SCRIPT_DIR)

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The LevelSequence actor without a Scene: the `.dseq` text (LevelSequenceAsset's static Write/Parse —
    -- the asset base is header-only), the registry-only entity host + actor state (LevelSequencePlayback),
    -- and the Timeline core they evaluate through. No renderer, no AssetManager.
    files {
        "level_sequence_main.cpp",
        "level_sequence_test.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/ECS/LevelSequencePlayback.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/ECS/LevelSequenceAuthoring.cpp",
        -- ANIM-LSEQ3: an Animation section poses the bound entity's Animator, so the Animator and what it
        -- links (graph, retarget, controls, rig — AnimatorPose's list) compile here.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Animator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/LayeredBlendPerBone.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraphInstance.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphEvaluator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphValidation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetSource.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TrackEditing.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/LevelSequenceAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Channel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Player.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Binding.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Track.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Sequence.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/SequenceFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Evaluator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",       -- the host applies to an entt registry
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the TMLN block and LoopMode's name are rfl
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

    -- Components.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs to know the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: JSON, canonical text, UUID, the logger. Optick: Common's JobSystem. Cocoa: Common's file dialog.
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
