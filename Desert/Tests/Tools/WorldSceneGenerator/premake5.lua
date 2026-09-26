local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool, a PREBUILD STEP of `Desert`. This suite asserts
    -- that the generated Settings block is exactly this build's SceneSettings field set, so a stale copy
    -- would certify yesterday's format.
    dependson { "Desert" }

    -- IT COMPILES THE TOOL, NOT A COPY OF IT. The suite drives Tools/WorldGen's own RunWorldGen so that
    -- what is asserted is the file the command line writes. Everything here is pure over parsed trees -
    -- no GPU, no window, no Scene, no AssetManager - which is what lets a world generator be a unit test.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Tools/WorldGen/Source/WorldBuild.cpp",
        "%{_MAIN_SCRIPT_DIR}/Tools/WorldGen/Source/WorldGenMain.cpp",
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source/SettingsCanonical.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Serialize/ForeignKeys.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Serialize/SceneFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",        -- The partitioner places a landscape tile by its root's frame (LS-3), and those two functions live
        -- in pure translation units that link only Common.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeLayout.cpp",
    }

    includedirs {
        -- LandscapeData.cpp compiles Shaders/Common/LandscapeHeight.glslh as C++.
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
        "%{_MAIN_SCRIPT_DIR}/Tools/WorldGen/Source",
        "%{_MAIN_SCRIPT_DIR}/Tools/SceneMigrator/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",        -- Components.hpp is an entt registry away
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",  -- the scene tree is rfl::Generic
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
        -- Common carries the Objective-C file dialog; linking it needs AppKit and the ObjC runtime. The
        -- suite reaches Common for the write primitive the tool writes the scene with.
        links { "Cocoa.framework", "Foundation.framework" }
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
