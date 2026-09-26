local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool, which runs as a PREBUILD STEP OF `Desert`. This
    -- suite round-trips the Settings block through the SAME reflection table the saver writes it with, so
    -- without this the parallel build could compile a stale copy and certify yesterday's field set.
    dependson { "Desert" }

    -- The whole point of K11 is that the merge is a PURE FUNCTION over two parsed trees: no GPU, no
    -- window, no Scene, no AssetManager. So the suite compiles the three files it needs and nothing else,
    -- and can therefore run the real corpus — every .desce in the repository — as a unit test.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/ForeignKeys.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/SceneFormat.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",        -- Components.hpp is an entt registry away
        "%{wks.location}/ThirdParty/reflect-cpp/include",  -- rfl::Generic and rfl::fields<>
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

    -- Engine headers reached through Components.hpp use DESERT_DEBUG_BREAK, which needs the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    links { "Common", "Optick" }

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
