local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- FlightRules.hpp is pure; the suite also drives the command line that arms a flight, and
    -- CommandLine.hpp checks --language against the locale table, which these two units provide.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocaleFormat.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/PluralRules.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source", -- <Engine/Localization/LocaleFormat.hpp>
        "%{wks.location}/Editor/Source",        -- <Editor/Core/FlightRules.hpp>, <Editor/Core/CommandLine.hpp>
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

    links { "Common", "Optick" }

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
