local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool as a PREBUILD STEP OF `Desert`. Without this edge
    -- a parallel build can compile a stale table in, and a stale table is precisely what this suite would
    -- then fail to notice: it exists to compare the reflection table against the scenes on disk.
    dependson { "Desert" }

    -- No migration TU any more. The protocol scenes must be at the CURRENT generation, and the check for
    -- that is Core::SceneIsAtCurrentVersion - an inline function in SceneFormat.hpp, which is the same gate
    -- the loader applies. The migrations themselves moved to Tools/SceneMigrator and are tested beside it.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- Components.hpp is an entt registry away
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the scene tree is rfl::Generic
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

    -- Components.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: UUID/AssetHandle/the logger the migration's warnings go through.
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
