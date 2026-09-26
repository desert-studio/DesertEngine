local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- Compile the unit under test directly (pure CPU; the rfl serialization .cpp is NOT needed here).
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphEvaluator.cpp",
        -- The evaluator delegates its structure check to the validator (ONE spelling of "which
        -- conditions name an undeclared parameter"), so the two units link together everywhere.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphValidation.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",  -- <Common/Core/ResultStr.hpp>
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",  -- <Engine/Animation/Graph/AnimGraph.hpp>
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
