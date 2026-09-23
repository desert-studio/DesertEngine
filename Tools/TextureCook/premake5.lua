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
        "%{wks.location}/Desert/Desert/Source/Engine/Project/ProjectContext.cpp",
        "%{wks.location}/Editor/Source/Editor/Import/TextureImporter.cpp",
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
        -- Common contains Objective-C (the file dialog); linking it needs AppKit + the ObjC runtime.
        links { "Cocoa.framework", "Foundation.framework" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}

    links { "Common", "Optick" } -- Common's JobSystem registers its workers with Optick

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
