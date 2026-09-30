local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- THE RULES, AND THE PLACEMENT RULE, compiled straight into the test. Both units are free of
        -- ImGui on purpose: the panel that draws them is the one place on this machine no test can run
        -- (synthetic input is closed, and a build machine has no `ed::EditorContext`), so the DECIDING
        -- was split out of it and the WIRING is asserted by a census over the panel's source text.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphValidation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphEvaluator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraph.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/GraphCanvas/GraphCanvas.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Panels/Animation/AnimGraphCanvasPlan.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Panels/Animation/PoseGraphEdit.cpp",
        -- ANIM-UI2: the graph's undo transaction, and the serializer that is its "did anything change".
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/Commands/AnimGraphEdit.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
    }

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

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
