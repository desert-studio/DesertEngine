local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- CommandLine.hpp is header-only and depends on nothing but glm (through ShotOptions), Common's
    -- ResultStr and -- since Ю15 -- the locale TABLE, which is a constexpr array of string literals. So
    -- the ENTIRE argument-parsing decision is still testable without a window, a device or a disk. That is
    -- the point of the file existing: the parse used to be a loop inside CreateApplication, where the only
    -- way to find out what an argument did was to launch the editor and look at what rendered.
    --
    -- `--language` is checked against that table AT PARSE TIME rather than at the point of use, so that a
    -- typo on an unattended run stops the run instead of rendering the default language under the name of
    -- the one that was asked for. The two localisation units below are what makes that check reachable
    -- here; neither pulls in a renderer.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocaleFormat.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/PluralRules.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source", -- <Engine/Localization/LocaleFormat.hpp>
        "%{wks.location}/Editor/Source",        -- <Editor/Core/CommandLine.hpp>
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
