local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The manipulator, the shape library, the control layer under them, the pose substrate under
        -- that, and the skeleton its spaces name. NO ImGui, NO Vulkan, NO Animator: the whole point of
        -- report 01 §833's immediate-mode layer is that drawing a control needs no render pass, and a
        -- suite that had to link one would be evidence against the design rather than for it.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlManipulator.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Rig/ControlShape.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
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
