local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The player's settings: Load / Save over real files in a temp directory, and Apply against a recording
    -- window, a real FramePacer, the real AudioMix and the real Localization service — the consumers, not a
    -- double of the settings module. No device, no audio device, no renderer.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Settings/GameUserSettings.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Audio/AudioMix.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/FramePacer.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/PluralRules.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocaleFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocalizedText.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/StringTable.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocalizationService.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the table is written by rfl::json
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

    -- Window.hpp reaches SwapChain.hpp -> Glfw.hpp and Device.hpp (types only, nothing is linked).
    for name, path in pairs(deps.Desert and deps.Desert.IncludeDir or {}) do
        externalincludedirs { path }
    end

    -- Common: StringTable stamps and reads the text asset header (StampTextHeader, AssetGuid).
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
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
