-- "Bone authoring state has an owner, and the ownership is CHECKED."
--
-- The unit under test is header-only and free of the renderer, ImGui and the engine:
-- Editor/Core/Selection/AuthoringContext.hpp is the election, and Editor/Core/EditorSubject.hpp (which it
-- includes) reaches nothing but Common. That is deliberate and is the property the four `static inline`
-- values it replaced could not offer -- a test builds its OWN AuthoringContextHost, so no assertion here
-- depends on what ran before it.
--
-- Two of the sections are CENSUSES over the repository's source text (the removed global is read nowhere;
-- only three named surfaces write the new one), so this suite also compiles the shared comment/literal
-- blanker from Desert/Tests/Engine/SettingConsumers. It is included by path rather than copied for the
-- reason that header's own top note gives: a scanner that mis-parses a character literal deletes the rest
-- of the file and answers "nobody reads this" in the same confident voice it uses when that is true.
--
-- Nothing from the engine or the editor is linked; `Common` is here for Common::UUID and the logger the
-- result type reaches for.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files { test_files }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
    }
    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.Common.Defines) do
        defines { define }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

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
