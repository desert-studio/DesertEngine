local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- One unit under test, GPU-free:
    --   * Editor/Resources/Shaders/Common/CloudField.glslh — the vertical profile, the coverage mapping
    --     and the depth-weighted erosion — compiled AS C++ through CloudFieldReference.hpp, fed by the
    --     same noise functions Programs/Clouds/CloudNoiseBake.shader bakes into the volume the march
    --     samples. That is why the SHADER ROOT is on the include path: the test drives the exact text the
    --     cloud passes compile, so a passing test is a statement about the code the GPU runs.
    -- Still no renderer and no Vulkan. The one engine source listed is the cloud TYPE's data layer, and
    -- only for CloudTypeDefaultShape: the coverage numbers this suite measures are calibration data for
    -- the shipped sky, so they have to be measured on the shape a scene with an empty slot actually
    -- renders rather than on a copy of it that can drift.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
        -- The PRODUCER, because the seam's field is a fetch of the volume this bakes. A suite that
        -- stubbed the volume would be measuring the sampler and nothing else; what makes the double
        -- compilation worth anything is that the bytes under the mirror are the bytes the device gets.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",
        -- The painted layout: CloudProceduralVolume.cpp reads it to decide a cell's coverage, so
        -- everything that compiles the bake compiles this too. It brings nothing with it -- no asset
        -- layer, no GPU, no filesystem -- which is the property that makes adding one line enough.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the type's file format is rfl::json
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

    -- Common: the Result/error type the type's validation is carried in.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- CloudProceduralVolume.cpp keys its bake in the DDC, whose file store is Common's FileSystem, which
    -- carries Objective-C (MacOSFileSystem's dialogs): the ObjC runtime + AppKit link too.
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
