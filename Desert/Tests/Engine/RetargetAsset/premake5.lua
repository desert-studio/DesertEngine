local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- A25. The load-bearing assertion of this suite is NOT "the bytes round-trip" — it is "a retarget
    -- reaches the SKINNING MATRICES", which is why the whole pose pipeline is compiled in: Animator, the
    -- control-rig stage it owns, the two-bone solver stage 3 calls, and the clip reader. The format alone
    -- would prove exactly what `PreloadCloudLayouts`'s own tests proved about a feature that was dead.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/Retarget.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Animator.cpp",
        -- The section blend the Animator samples through (AnimationClip::SampleTrack). A header-inline
        -- call into a .cpp nobody linked is a LINK error and not a silent wrong answer, which is why
        -- ApplySection lives in a translation unit rather than in the header beside its caller.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/ClipSection.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetSource.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",
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

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    links { "Common", "Optick" }

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
