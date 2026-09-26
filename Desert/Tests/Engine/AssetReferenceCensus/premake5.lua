local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The test cpp plus TextureSourceAsset.cpp: a `.detex` states its handle in its header (AF3), read by
    -- ReadTextureAssetKey. The rest is header-only: MaterialData is the `.demat` aggregate, AssetHandle
    -- carries the path derivation, and Constants::Path the root table. Nothing here touches the renderer,
    -- so the suite runs on a checkout with no cooked tree at all -- which is the point of it.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp",
        -- A mesh is a MeshSourceAsset (AF4d): its header states the MSAS subsystem, and its material slots
        -- are references this census counts, read by the engine's own ReadMeshSourceAssetFile.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include",
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

    links { "Common", "Optick" } -- Commons JobSystem registers worker threads with Optick

    filter "system:not windows"
        links { "ReflectCpp" }

    -- ReadTextureAssetKey reads through Common::Utils::FileSystem, whose macOS half is Objective-C
    -- (MacOSFileSystem's file dialog), so the ObjC runtime + AppKit link as well.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }

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
