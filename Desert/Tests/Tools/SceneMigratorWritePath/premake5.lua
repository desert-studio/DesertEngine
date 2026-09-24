local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Unlike the suites beside it, this one compiles the TOOL and not only the migrations: the claim
    -- under test is the file loop itself (read, write back atomically, exit code), which lives in
    -- MigratorMain.cpp behind RunSceneMigrator. SceneMigration.cpp comes along because the loop
    -- calls MigrateScene on every file it parses.
    files {
        test_files,
        "%{wks.location}/Tools/SceneMigrator/Source/MigratorMain.cpp",
        -- A5 gave the tool a fourth file class (`.anim`), so the suite that compiles its main must link
        -- the conversion it now calls, and the tick model that conversion targets. Both are pure.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipMigrate.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{wks.location}/Tools/SceneMigrator/Source/SceneMigration.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp", -- the v22 -> v23 step bakes tiles
        -- The anim graph's JSON round trip: schema step 21 moves the state machine out of the entity and
        -- reads it with the engine's own parser, so every suite that compiles the migration links it too.
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
        -- The tool's loop canonicalises the Settings block through the ENGINE'S reflection table, so a
        -- suite that compiles MigratorMain.cpp has to bring the table with it. It is deliberately not in
        -- SceneMigration.cpp: the fifteen suites that test one schema step each must stay free of it.
        "%{wks.location}/Tools/SceneMigrator/Source/SettingsCanonical.cpp",
        -- The loop's material step (MATL 2) and scene step (SCNE 27) read the legacy-id register.
        "%{wks.location}/Tools/SceneMigrator/Source/LegacyMaterialIds.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        -- The loop raises `.deprefab` files too (И11), and the bytes it writes for one come from the
        -- ENGINE'S own writer, gate-checked by the ENGINE'S own loader gate — so a suite that compiles
        -- the loop has to bring that pair with it, for the same reason it brings the reflection table.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp",
    }

    -- Reflection.gen.cpp is emitted by DesertHeaderTool as a prebuild step of `Desert`.
    dependson { "Desert" }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        -- The migrations and the tool's loop live in the TOOL (see SceneMigratorEndToEnd's premake
        -- for why they moved out of the engine). This resolves <MigratorMain.hpp> and
        -- <SceneMigration.hpp>, and their own includes of themselves.
        "%{wks.location}/Tools/SceneMigrator/Source",
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

    -- Common: UUID/AssetHandle/the logger + the atomic write primitive the tool's loop goes through.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- Common contains Objective-C (MacOSFileSystem file dialog) — referencing FileSystem pulls it in,
    -- so the ObjC runtime + AppKit must link too.
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
