local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The REAL packer and the real project context, not mirrors: the suite calls BuildContentPak()
    -- over a temp project and then plays the packaged game's side of the mount. The packer now COOKS
    -- before it packs (PackageCook), so the real cook comes too: the shader compiler (shaderc, no
    -- VkDevice — the same recipe as Tests/Engine/PBRSceneFrame), the font baker (stb_truetype) and
    -- the icon baker, each with its cache seam. Still nothing from the renderer.
    -- THE PLAYER'S OWN DISCOVERY, compiled in beside the packer (П5). What a package is and what a
    -- player looks for are decided in two different binaries, so the only place their agreement can be
    -- asserted is a test that holds both — and the disagreement this closes (a descriptor nothing
    -- wrote, discovered by nothing) survived precisely because nothing linked the two ends together.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Runtime/Source/PackagedContent.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Packaging/GamePackager.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Packaging/PackageCook.cpp",
        -- GamePackager cuts a partitioned world into its cells (WP9): the world cook and what it reads with.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Serialize/WorldCells.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/FoliageType.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Serialize/SceneFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Serialize/ExternalEntities.cpp", -- a partitioned world is read joined (WP16)
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Serialize/ForeignKeys.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeLayout.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Project/ProjectContext.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderCompiler.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderCacheKey.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderSpirvCache.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/Includer/ShaderIncluder.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Text/FontBaker.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Text/Msdf.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Text/FontCache.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Vector/VectorImage.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Vector/IconBake.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_truetype.cpp",
        -- THE TEXTURE COOK (PK1): the editor's importer itself, the container it writes, the BC7 gates it
        -- measures its own output against, and stb_image as the one decoder in the closure. The same
        -- four files Tests/Editor/TextureImport compiles, so nothing here is a second texture cook.
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/TextureImporter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_image.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/tinyexr/tinyexr.cpp", -- .exr sources (links stb_image.cpp for deflate)
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
        "%{_MAIN_SCRIPT_DIR}/Runtime/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/include", -- <stb_image/stb_image.h>, for the texture cook
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/tinyexr/include", -- <tinyexr/tinyexr.h>, for the texture cook
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    -- Vulkan headers + shaderc, exactly as Tests/Engine/PBRSceneFrame pulls them.
    for name, path in pairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    -- DESERT_DEBUG_BREAK needs the platform macro; any engine header reaching DESERT_VERIFY fails to
    -- compile without it (same three lines every engine-linking test carries).
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- The engine defines this for its own Debug TUs; the shader-compiler TUs compiled INTO this test
    -- must see the same value or the cook here would key artifacts differently from the engine built
    -- in the same configuration.
    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
    filter {}

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    links { "Common", "Optick" } -- Commons JobSystem registers worker threads with Optick

    -- Common contains Objective-C (MacOSFileSystem file dialog) — pulled in because this test
    -- references FileSystem, so the ObjC runtime + AppKit must link too.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    filter "configurations:Debug"
        for name, path in pairs(deps.TestSpecific.Libraries.Debug) do
            links { path }
        end
        for name, path in pairs(deps.DesertSpecific.Libraries.Debug) do
            links { path }
        end

    filter "configurations:Release"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end
        for name, path in pairs(deps.DesertSpecific.Libraries.Release) do
            links { path }
        end

    filter {}

print("Configured test project: " .. test_name)
