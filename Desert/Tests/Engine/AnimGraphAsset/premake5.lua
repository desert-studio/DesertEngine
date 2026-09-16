local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The asset wrapper and the JSON round trip it parses with. The EVALUATOR is here too, because
        -- this suite's load-bearing assertion is not "the bytes round-trip" -- it is that one file becomes
        -- ONE object that several entities share, and the thing that consumes that object is an evaluator.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/AnimGraphAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphEvaluator.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }

    -- LINKED, not compiled in: SaveControlRigFile goes through Common's atomic write primitive, so the
    -- file half of the format is exercised by the same call the editor makes rather than by a local
    -- ofstream this suite invents. Common carries Objective-C (the macOS file dialog), which is why the
    -- two frameworks come with it — FileSystemWrite's premake makes the same pair for the same reason.
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
