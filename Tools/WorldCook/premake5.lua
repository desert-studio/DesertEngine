-- WorldCook — a partitioned world's source (.desce with a WorldPartition block) into one file per cell and a
-- world index (Engine/Core/Serialize/WorldCells.hpp, WP8).
--
-- A TOOL OF ITS OWN, not a mode of WorldGen or SceneMigrator, because it is neither of their jobs. WorldGen
-- AUTHORS a synthetic source world; SceneMigrator converts a source file IN PLACE to the current schema. The
-- cook reads any source world — generated or saved by the editor — and writes a DERIVED tree beside it that
-- the runtime streams from (WP9), the same place in the pipeline as TextureCook and PakTool. What it does is
-- in the engine (WorldCells.cpp) because the runtime reads what it writes with the same code; this binary is
-- arguments, files and a report.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "WorldCook"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/WorldCells.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/SceneFormat.cpp",
        "Source/**.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/WorldCells.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/ForeignKeys.cpp",
        -- The partitioner places a landscape tile by its root's frame (LS-3); both files are pure and link
        -- only Common.
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeLayout.cpp",
    }

    includedirs {
        -- LandscapeData.cpp compiles Shaders/Common/LandscapeHeight.glslh as C++.
        "%{wks.location}/Editor/Resources/Shaders",
        "%{wks.location}/Tools/Shared",
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
