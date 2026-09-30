local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The asset wrapper and the JSON round trip it parses with. The EVALUATOR is here too, because
        -- this suite's load-bearing assertion is not "the bytes round-trip" -- it is that one file becomes
        -- ONE object that several entities share, and the thing that consumes that object is an evaluator.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/AnimGraphAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphEvaluator.cpp",
        -- The evaluator delegates its structure check to the validator (ONE spelling of "which
        -- conditions name an undeclared parameter"), so the two units link together everywhere.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphValidation.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    -- LINKED, not compiled in: SaveControlRigFile goes through Common's atomic write primitive, so the
    -- file half of the format is exercised by the same call the editor makes rather than by a local
    -- ofstream this suite invents. Common carries Objective-C (the macOS file dialog), which is why the
    -- two frameworks come with it — FileSystemWrite's premake makes the same pair for the same reason.
    links { "Common", "Optick" }

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
