-- GamePackager — the player's artifact, built with no editor and no GPU.
--
-- THE FILE LIST IS NOT A DESIGN, IT IS A COPY OF ONE THAT IS ALREADY GREEN. Every entry below is in
-- Desert/Tests/Editor/PackagedContent/premake5.lua, which has been calling the real PackageGame()
-- inside a gtest binary on both platforms for weeks — that suite is the evidence that the packer
-- and its cook need no device, and this tool is that same closure with a main() instead of a test
-- (and without Runtime/Source/PackagedContent.cpp, which is the PLAYER's half).
--
-- What it links that AssetClosure does not: shaderc and spirv-cross, because packaging COOKS. No
-- VkDevice is ever created — `DesertSpecific` carries the Vulkan loader only because shaderc sits
-- beside it in the SDK, and the one Vulkan token in the compiled path is a shaderc target-env enum.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "GamePackager"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        "%{wks.location}/Editor/Source/Editor/Packaging/GamePackager.cpp",
        "%{wks.location}/Editor/Source/Editor/Packaging/PackageCook.cpp",
        -- The packager cuts a partitioned world into its cells (WP9): the world cook and what it reads with.
        -- The same four files Tests/Editor/PackagedContent compiles, so the suite links what the tool links.
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/WorldCells.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/SceneFormat.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeLayout.cpp",
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
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
        "%{wks.location}/ThirdParty/stb/stb_image.cpp",
    }

    includedirs {
        "%{wks.location}/Tools/Shared",
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
    }

    externalincludedirs {
        "%{wks.location}/ThirdParty/stb/include", -- <stb_image/stb_image.h>, for the texture cook
        -- <Common/LandscapeHeight.glslh>: LandscapeData.cpp decodes heights with the shader's own maths.
        "%{wks.location}/Editor/Resources/Shaders",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    -- DESERT_DEBUG_BREAK needs the platform macro; any engine header reaching DESERT_VERIFY fails to
    -- compile without it.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        -- Common contains Objective-C (the file dialog); linking it needs AppKit + the ObjC runtime.
        links { "Cocoa.framework", "Foundation.framework" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}

    links { "Common", "Optick" } -- Common's JobSystem registers its workers with Optick

    filter "system:not windows"
        links { "ReflectCpp" }

    filter {}

    -- The engine defines this for its own Debug TUs, and the shader-compiler TUs compiled in here
    -- must see the same value: otherwise this tool would key cooked SPIR-V differently from the
    -- engine built in the same configuration, and every artifact it produced would be a cache miss.
    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        symbols "On"
        for name, path in pairs(deps.DesertSpecific.Libraries.Debug) do
            links { path }
        end

    -- `or Shipping`: there is no third flavour of prebuilt third-party libraries, and inventing one
    -- would mean pinning Vulkan and reflect-cpp twice (the argument Runtime/premake5.lua makes).
    -- Without the `or`, the filter matches nothing in Shipping and the link is thousands of
    -- unresolved shaderc symbols -- which is exactly how the 257 test scripts failed that build.
    filter "configurations:Release or Shipping"
        optimize "On"
        for name, path in pairs(deps.DesertSpecific.Libraries.Release) do
            links { path }
        end

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }

    filter {}
