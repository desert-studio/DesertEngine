-- GamePackager — the player's artifact, built with no editor UI and no GPU.
--
-- THE ENGINE IS LINKED, NOT COPIED. The packer and its cook are four editor translation units (below); what they
-- call is the engine, linked as every engine user links it: one library per module through
-- DesertModuleLibs.Link() (BuildScripts/DesertModuleLibs.lua), so the archives carry their own closure and the
-- linker takes only the members the packager reaches.
--
-- It used to compile a hand-kept list of ~30 engine sources instead, "a copy of" the PackagedContent suite's
-- premake. BUILD1 folded that suite into the Editor runner, which links the whole engine, so nothing held the
-- list to the code any more: when the packaged cook started building HLODs with the engine's builders
-- (Engine/Core/HLODMeshBuilder.cpp: MakeHLODMeshBuilder, MakeLandscapeHLODBuilder - WP-FAR-6/7) the list did not
-- grow and the tool stopped linking.
--
-- The editor half is compiled here, not taken from EditorCore: the Shipping CI leg builds Runtime + this tool
-- and no editor, and linking EditorCore would compile the whole editor into that leg for four files.
--
-- No VkDevice is ever created: the Vulkan loader is on the link line because shaderc sits beside it in the
-- SDK and the engine archives hold the renderer, not because packaging touches a GPU.
local engineDeps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "GamePackager"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Packaging/GamePackager.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Packaging/PackageCook.cpp",
        -- THE TEXTURE COOK (PK1): the editor's importer and the .dds source decoder it calls (bcdec inside).
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/TextureImporter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/DdsSource.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/Shared",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/UI/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Render2DCore/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/CoreReflection/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
    }

    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/include", -- <stb_image/stb_image.h>, for the texture cook
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/openexr/src/lib/OpenEXRCore", -- <openexr.h> (OpenEXRCore), for the texture import
        "%{_MAIN_SCRIPT_DIR}/build/generated/openexr/include",  -- its generated config headers (BuildScripts/ThirdParty/OpenEXR.lua)
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/Imath/src/Imath",
    }

    for name, path in DesertSortedPairs(engineDeps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in DesertSortedPairs(engineDeps.DesertSpecific.IncludeDir) do
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

    -- The engine defines this for its own Debug TUs; Main.cpp sees the same engine headers the libraries
    -- were compiled with.
    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        symbols "On"

    filter "configurations:Release or Shipping"
        optimize "On"

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }

    filter {}

    -- What the engine archives use, as Runtime/premake5.lua names it, plus OpenEXRCore for the texture import.
    links {
        "GLFW",
        "Optick",
        "MeshOptimizer",
        "Voro",
        "OpenSubdiv",
        "OpenEXRCore", -- .exr texture sources (BuildScripts/ThirdParty/OpenEXR.lua)
        "Dav1d",
        "Opus",
    }
    -- The engine, one library per module, then the prebuilt archives (Vulkan, shaderc, SPIRV-Cross,
    -- reflect-cpp) per configuration, Shipping included.
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
