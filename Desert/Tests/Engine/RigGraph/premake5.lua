local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The walk under test; the stage that runs it and decides what reaches a bone; the hierarchy it
        -- reads and writes; and the FORMAT, because the graph's refusals have to be askable from a file as
        -- well as from a built rig — one walk, two callers, and this suite is where they are made to agree.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/ControlRig.cpp",
        -- The pipeline the rig joins: without it the suite could say "the stage ran" and not "the skinning
        -- matrices came out different", which is the only statement that means the graph reached a frame.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Animator.cpp",
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

    -- LINKED for ControlRigAsset's reason: the format's file half goes through Common's atomic write, and
    -- Common carries Objective-C (the macOS file dialog), which is what the two frameworks are for.
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
