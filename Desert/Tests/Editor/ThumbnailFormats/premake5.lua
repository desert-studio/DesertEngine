-- "Every format the Content Browser can show either has a thumbnail producer or a named reason why not."
--
-- The census half is READ FROM THE SOURCES (the browser's own s_FileTypes literal), the way
-- ThumbnailRequesters and AssetPreloadCensus next door are. The other half is not a reading at all: every
-- Producer::Painted row is PAINTED HERE against the shipped asset library, which is why the four cloud
-- containers' decoders are compiled in. They are listed rather than linked because libDesert pulls in
-- Vulkan and the whole renderer, and not one of these four translation units needs a device — which is
-- precisely why a cloud thumbnail can be produced on a JobSystem worker in the first place.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Widgets/CloudThumbnail.cpp",
        "%{wks.location}/Editor/Source/Editor/Widgets/HdrSphereThumbnail.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
        -- The UI theme format: CloudThumbnail paints a `.detheme` by parsing it, so the parser is part of
        -- what this suite exercises.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/UIThemeData.cpp",
        -- STB_IMAGE_WRITE_IMPLEMENTATION lives here; CloudThumbnail::Write streams the PNG through it.
        "%{wks.location}/ThirdParty/stb/stb_image.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    -- stb (for stb_image_write.h) and reflect-cpp (the .decloudtype reader) come from the engine's own
    -- dependency group; the test group carries neither.
    externalincludedirs {
        deps.DesertSpecific.IncludeDir.stb,
        deps.DesertSpecific.IncludeDir.base,
        deps.DesertSpecific.IncludeDir.entt,
    }

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    links { "Common", "Optick" } -- the modelling bake parallelises through Common's JobSystem

    -- Common contains Objective-C (MacOSFileSystem's file dialog); linking Common pulls it in.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
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
