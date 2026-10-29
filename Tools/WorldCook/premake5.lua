-- WorldCook — a partitioned world's source (.desce with a WorldPartition block) into one file per cell and a
-- world index (Engine/Core/Serialize/WorldCells.hpp, WP8).
--
-- A TOOL OF ITS OWN, not a mode of WorldGen or SceneMigrator, because it is neither of their jobs. WorldGen
-- AUTHORS a synthetic source world; SceneMigrator converts a source file IN PLACE to the current schema. The
-- cook reads any source world — generated or saved by the editor — and writes a DERIVED tree beside it that
-- the runtime streams from (WP9), the same place in the pipeline as TextureCook and PakTool. What it does is
-- in the engine (WorldCells.cpp) because the runtime reads what it writes with the same code; this binary is
-- arguments, files and a report.
--
-- THE ENGINE IS LINKED, NOT COPIED (WP-FAR-6B), as GamePackager links it: one library per module through
-- DesertModuleLibs.Link() (BuildScripts/DesertModuleLibs.lua). The cook builds the cells' HLODs with the
-- engine's builders (Engine/Core/HLODMeshBuilder.hpp: MakeHLODBuilders - DynamicMesh, the QEM simplifier, the
-- landscape tile reader), the same ones editor Play and the packager use; their closure is most of Geometry
-- and Assets, and a hand-kept source list would miss the next file the way GamePackager's did. No VkDevice is
-- ever created: the Vulkan loader is on the link line because the engine archives hold the renderer.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "WorldCook"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/Shared",
    }

    for name, path in DesertSortedPairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in DesertSortedPairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    -- DESERT_DEBUG_BREAK needs the platform macro; any engine header reaching DESERT_VERIFY fails to
    -- compile without it.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}

    -- The engine defines this for its own Debug TUs; the tool sees the same engine headers the libraries
    -- were compiled with.
    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        symbols "On"

    filter "configurations:Release or Shipping"
        optimize "On"

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }

    filter {}

    -- What the engine archives use, as Runtime/premake5.lua names it.
    links {
        "GLFW",
        "Optick",
        "MeshOptimizer",
        "Voro",
        "MikkTSpace",
        "OpenSubdiv",
        "Embree", -- BuildScripts/ThirdParty/Embree.lua: the distance-field builder
        "OpenEXRCore",
        "Dav1d",
        "Opus",
    }
    -- The engine, one library per module, then the prebuilt archives (Vulkan, shaderc, SPIRV-Cross,
    -- reflect-cpp) per configuration.
    DesertModuleLibs.Link()

    -- gmake does not link a static library's dependencies transitively (Visual Studio does, through the
    -- project references): the executable names what the engine libraries use, as the Editor does.
    filter "system:macosx"
        links {
            "Common",
            "Jolt",
            "LuauCodeGen",
            "LuauCompiler",
            "LuauAst",
            "LuauBytecode",
            "LuauVM",
            "LuauCommon",
            "ReflectCpp",
            "Cocoa.framework",
            "IOKit.framework",
            "CoreFoundation.framework",
            "CoreVideo.framework",
            "CoreMedia.framework",
            "AVFoundation.framework",
            "QuartzCore.framework",
            "Foundation.framework",
        }

    filter {}
