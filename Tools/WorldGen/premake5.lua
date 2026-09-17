-- WorldGen — the generator of the programme's world-scale measuring stick (Docs/World/PROGRAMME.md §1).
--
-- It is a TOOL and not a script because the file it writes has to be one the engine's loader accepts and
-- one the engine's saver would have written: it builds Core::SceneSerialized and Assets::EntityData and
-- lets rfl write them, so there is no second statement of the .desce format anywhere. A JSON-emitting
-- script would be exactly the fork Tools/SceneMigrator's own header forbids.
--
-- Like SceneMigrator it reaches into the engine for HEADERS and for the two things that must not be
-- restated — the current on-disk struct and the reflection table the Settings block is written through.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "WorldGen"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",

        -- THE SETTINGS BLOCK, WRITTEN BY THE ENGINE'S OWN TABLE. SceneMigrator already owns the one
        -- function that turns "no Settings block" into the exact bytes the saver produces; a second copy
        -- here would be a second statement of a 51-field format. Reflection.gen.cpp is emitted by
        -- DesertHeaderTool as a PREBUILD STEP of `Desert`, hence the dependson below.
        "%{wks.location}/Tools/SceneMigrator/Source/SettingsCanonical.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
    }

    dependson { "Desert" }

    includedirs {
        "%{wks.location}/Tools/Shared",
        "%{wks.location}/Tools/SceneMigrator/Source",
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include",         -- PrefabData reaches ECS headers
        "%{wks.location}/ThirdParty/reflect-cpp/include",  -- the scene tree is rfl::Generic
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    links { "Common", "Optick", "ReflectCpp" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        -- Common contains Objective-C (file dialog); linking it needs AppKit + the ObjC runtime.
        links { "Cocoa.framework", "Foundation.framework" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}
