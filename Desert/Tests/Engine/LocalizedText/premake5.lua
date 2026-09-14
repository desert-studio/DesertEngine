local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The whole of localisation EXCEPT the asset wrapper: the `.destrings` format, the sigil that tells a
    -- key from a literal, the form ladder, the placeholder grammar, and the service the UI resolves
    -- through. No renderer, no asset manager and no device — which is why the decisive relation (a
    -- language change moves a key and cannot move a literal) is assertable here and not only in a frame.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/PluralRules.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocaleFormat.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocalizedText.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/StringTable.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocalizationService.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the table is written by rfl::json
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
