local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- THE SHARED CANVAS LAYER AND BOTH DOCUMENTS' PLAN BUILDERS, compiled straight into the test.
        -- That is the whole reason those three units exist apart from their panels: a canvas identity
        -- defect cannot be reached through an `ed::EditorContext` on a build machine, and the defect this
        -- suite measures (an index used as an identity) lived inside a panel's draw call for that reason.
        "%{wks.location}/Editor/Source/Editor/Core/GraphCanvas/GraphCanvas.cpp",
        "%{wks.location}/Editor/Source/Editor/Panels/Animation/AnimGraphCanvasPlan.cpp",
        "%{wks.location}/Editor/Source/Editor/Panels/NodeGraph/ShaderGraphCanvasPlan.cpp",
        -- The two FILE formats, so the fingerprints below are taken over the graphs this project ships
        -- rather than over graphs the test invented for itself.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/ShaderGraph.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
    }

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

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
