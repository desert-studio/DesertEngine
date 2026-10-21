-- GamePackager — the player's artifact, built with no editor UI and no GPU.
--
-- THE ENGINE AND THE EDITOR'S COOK ARE LINKED, NOT COPIED. What the packer calls is the engine, linked as every
-- engine user links it: one library per module through DesertModuleLibs.Link() (BuildScripts/DesertModuleLibs.lua),
-- so the archives carry their own closure and the linker takes only the members the packager reaches. The packer
-- itself and its cook (Editor/Packaging/GamePackager.cpp, PackageCook.cpp) and everything they call in the editor
-- (the texture importer, the mesh builders: Editor/Import/MeshDeriver.cpp BuildMeshPlatformData,
-- Editor/Import/ImportManager.cpp BuildMeshSourceOnMiss and the importers behind it) come from the editor's
-- library, EditorCore, linked as an ordinary archive — NOT whole-archive, as the Editor executable links it: the
-- component editors' static registrars are not reached, so their members (and the UI they draw) stay out.
--
-- It used to compile hand-kept lists instead: ~30 engine sources (they stopped linking when the cook started
-- building HLODs with Engine/Core/HLODMeshBuilder.cpp - WP-FAR-6/7), then four editor sources (they stopped
-- linking when the cook started deriving every static mesh with MeshDeriver/ImportManager - WP-FAR-6B pass 9n).
-- A list does not grow with the code it names; a library does.
--
-- The price: a build of this tool builds EditorCore, so the Shipping leg (Runtime + this tool) compiles the editor
-- library too. Correct closure over a shorter leg — the cook IS editor code.
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

    -- What EditorCore's reached members and the engine archives use, as the Editor names it.
    links {
        -- The packer, its cook and the importers they call (header above); first, so gmake resolves it before what
        -- it uses. The third-party libraries after it are the ones the reached importer members use.
        "EditorCore",
        "Assimp", -- the mesh importer behind BuildMeshSourceOnMiss (BuildScripts/ThirdParty/Assimp.lua)
        "ImGui",
        "ImGuiNodeEditor",
        "GLFW",
        "Optick",
        "MeshOptimizer",
        "Voro",
        "MikkTSpace",
        "OpenSubdiv",
        "OpenEXRCore", -- .exr texture sources (BuildScripts/ThirdParty/OpenEXR.lua)
        "Dav1d",
        "Opus",
    }
    -- The engine, one library per module, then the prebuilt archives (Vulkan, shaderc, SPIRV-Cross,
    -- reflect-cpp) per configuration, Shipping included.
    DesertModuleLibs.Link()
    -- EditorCore is compiled with dlib when the sources are present (Editor/premake5.lua); its archive then names it.
    if os.isdir( _MAIN_SCRIPT_DIR .. "/ThirdParty/dlib" ) then
        links { "Dlib" }
    end

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
