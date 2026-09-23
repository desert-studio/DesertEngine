local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The importer ITSELF, not a copy of its rules. TextureImporter.cpp reaches nothing but std::filesystem,
    -- stb_image and the cooked-path formula, so the file every texture in the project is cooked by can be
    -- compiled into a test binary as-is - which is the only way a defect planted in it comes out red.
    -- stb_image.cpp is the implementation TU for the header the importer includes.
    -- TextureAsset.cpp is the READ side of the .tex the importer writes: the cross-checkout round trip
    -- ("a .tex cooked here loads its pixels in a differently-rooted checkout") is a relation between the
    -- two, so both have to be the real code, in one binary.
    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Import/TextureImporter.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp", -- TextureAsset reads the cooked container through it
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp", -- the cook measures its own BC7 output before keeping it
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/TextureAsset.cpp",
        "%{wks.location}/ThirdParty/stb/stb_image.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source", -- <Engine/Assets/Serialization/TextureBinary.hpp>
        "%{wks.location}/Editor/Source",        -- the importer's own "TextureImporter.hpp" / "CookPaths.hpp"
        "%{wks.location}/Editor/Source/Editor/Import",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/", -- AssetManager.hpp, included by TextureAsset.hpp
        "%{wks.location}/ThirdParty/stb/include",
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the .tex payload is written with rfl::json
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: UUID/AssetHandle and the FileSystem helpers. Optick: Common's JobSystem registers its
    -- worker threads with the profiler.
    links { "Common", "Optick" }

    -- Common contains Objective-C (MacOSFileSystem's file dialog) and TextureAsset::Load reaches
    -- Common::Utils::FileSystem, so the ObjC runtime + AppKit have to link as well.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    filter "system:not windows"
        links { "ReflectCpp" }
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
