local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- Units under test (pure CPU): the .anim channel list -> runtime clip conversion, and since Д35
        -- its mirror -- runtime clip -> .anim, plus the file write that used to live inside an ImGui
        -- panel and therefore could not be compiled into any test binary at all.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        -- The conversion OUT of generation 0, and the tick grid it converts into (A5). Both are pure, so
        -- the suite that owns the format also owns its migration without gaining a dependency.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TrackEditing.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- <rflcpp/rfl.hpp>: the .anim format IS these structs
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

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

    -- AnimationClipWrite.cpp reaches Common::Utils::FileSystem, and Common's file dialog is Objective-C,
    -- so the ObjC runtime + AppKit link too. (That link cost is also the argument recorded in
    -- Tools/FbxMeshSplitter for why that one tool keeps a local close-and-check instead.)
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    filter "system:not windows"
        links { "ReflectCpp" }

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
