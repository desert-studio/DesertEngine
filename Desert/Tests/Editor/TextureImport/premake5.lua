local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The importer ITSELF, not a copy of its rules. TextureImporter.cpp reaches nothing but std::filesystem,
    -- stb_image and the cooked-path formula, so the file every texture in the project is cooked by can be
    -- compiled into a test binary as-is - which is the only way a defect planted in it comes out red.
    -- stb_image.cpp is the implementation TU for the header the importer includes.
    -- TextureAsset.cpp is the READ side of the .tex the importer writes: the cross-checkout round trip
    -- ("a .tex cooked here loads its pixels in a differently-rooted checkout") is a relation between the
    -- two, so both have to be the real code, in one binary.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/TextureImporter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp", -- TextureAsset reads the cooked container through it
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp", -- the cook measures its own BC7 output before keeping it
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/TextureAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_image.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/tinyexr/tinyexr.cpp", -- .exr sources (links stb_image.cpp for deflate)
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source", -- <Engine/Assets/Serialization/TextureBinary.hpp>
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",        -- the importer's own "TextureImporter.hpp" / "CookPaths.hpp"
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/", -- AssetManager.hpp, included by TextureAsset.hpp
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/include",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/tinyexr/include", -- <tinyexr/tinyexr.h>, for the texture cook
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the .tex payload is written with rfl::json
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
