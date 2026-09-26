local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- ImportUnits.cpp IS the subject, compiled here rather than restated, so a change to the rule
    -- reaches this suite. It is deliberately assimp-free: the rule is a function of what the file said
    -- and what extension it has, and keeping it that way is what lets a unit test cover it at all
    -- without dragging the FBX parser in. The suite also reads AssimpImporters.txt — the register of
    -- the formats this engine ships — so the rule and the register cannot drift apart.
    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Import/ImportUnits.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",  -- <Common/Core/Units.hpp>: the centimetre convention
        "%{wks.location}/Editor/Source",         -- <Editor/Import/ImportUnits.hpp>
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

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: nothing of it is called, but gtest's main links against the workspace's standard set and
    -- Optick is what Common's JobSystem registers its threads with.
    links { "Common", "Optick" }

    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    filter "system:not windows"
        links { "ReflectCpp" }
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
