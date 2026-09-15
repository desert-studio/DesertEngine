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
        -- Animator.cpp runs the Controls stage, so it needs the control base it calls through. The base is
        -- two functions and no Vulkan; no suite here adds a control, which is what makes "a rig with no
        -- controls behaves exactly as before" a thing these suites stillmeasure.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp",
        -- Timestep's constructor lives in a .cpp, and Animator::Update takes one. libCommon is not among
        -- the libraries a test suite links (only gtest and the reflect-cpp/optick shims are), so the one
        -- translation unit that defines it has to be listed here.
        "%{wks.location}/Desert/Common/Source/Common/Core/Timestep.cpp",
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
