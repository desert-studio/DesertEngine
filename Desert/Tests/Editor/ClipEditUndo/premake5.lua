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
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Core/Commands/PoseEditTransaction.cpp",

        -- The keying half of the round trip (same list as Desert/Tests/Engine/ControlKeying).
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlKeyer.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TrackEditing.cpp",

        -- The pose half. Animator.cpp is pure CPU (no Vulkan symbols), and every suite that compiles it
        -- links the retarget/rig/solver layer it owns -- enumerated because premake lists sources
        -- explicitly, so a real-but-unlisted edge fails as an undefined symbol naming a file that is
        -- sitting right there, which reads as a defect in the merge.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Animator.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/ClipSection.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetSource.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",

        "%{wks.location}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
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
