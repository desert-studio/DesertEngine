local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

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
        "%{wks.location}/Runtime/Source/PackagedContent.cpp",
        "%{wks.location}/Editor/Source/Editor/Packaging/GamePackager.cpp",
        "%{wks.location}/Editor/Source/Editor/Packaging/PackageCook.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Project/ProjectContext.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderCompiler.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderCacheKey.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderSpirvCache.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/Includer/ShaderIncluder.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Text/FontBaker.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Text/Msdf.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Text/FontCache.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Vector/VectorImage.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Vector/IconBake.cpp",
        "%{wks.location}/ThirdParty/stb/stb_truetype.cpp",
        -- THE TEXTURE COOK (PK1): the editor's importer itself, the container it writes, the BC7 gates it
        -- measures its own output against, and stb_image as the one decoder in the closure. The same
        -- four files Tests/Editor/TextureImport compiles, so nothing here is a second texture cook.
        "%{wks.location}/Editor/Source/Editor/Import/TextureImporter.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
        "%{wks.location}/ThirdParty/stb/stb_image.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
        "%{wks.location}/Runtime/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include",
        "%{wks.location}/ThirdParty/stb/include", -- <stb_image/stb_image.h>, for the texture cook
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

    filter "system:not windows"
        links { "ReflectCpp" }

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
