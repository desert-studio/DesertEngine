-- TextureCook — cooks named texture sources into a project's Cooked/ tree, from the shell.
--
-- It exists for the one texture a SCRIPT has to cook: the editor's start-up splash, which
-- scripts/MacOS/Package.sh cooks into the engine drop so that the drop's first start already has its
-- picture (Editor/Splash/SplashImage.hpp). It is NOT a second texture cook: it compiles the editor's own
-- TextureImporter — the same four files Tests/Editor/TextureImport and GamePackager compile — and calls
-- `TextureImporter::Cook`, so freshness, intent (`.detex`), BC7 and the container are the editor's.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "TextureCook"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Project/ProjectContext.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/TextureImporter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_image.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/Shared",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
    }

    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/include", -- <stb_image/stb_image.h>, for the texture cook
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/openexr/src/lib/OpenEXRCore", -- <openexr.h> (OpenEXRCore), for the texture import
        "%{_MAIN_SCRIPT_DIR}/build/generated/openexr/include",  -- its generated config headers (BuildScripts/ThirdParty/OpenEXR.lua)
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/Imath/src/Imath",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}

    links { "Common", "Optick" } -- Common's JobSystem registers its workers with Optick
    links { "OpenEXRCore" } -- .exr texture sources (BuildScripts/ThirdParty/OpenEXR.lua)

    filter "system:not windows"
        links { "ReflectCpp" }

    filter {}

    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        symbols "On"
        for name, path in pairs(deps.DesertSpecific.Libraries.Debug) do
            links { path }
        end

    filter "configurations:Release or Shipping"
        optimize "On"
        for name, path in pairs(deps.DesertSpecific.Libraries.Release) do
            links { path }
        end

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }

    filter {}
