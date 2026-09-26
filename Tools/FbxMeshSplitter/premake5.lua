-- FbxMeshSplitter — standalone CLI: splits a multi-mesh FBX into individual .obj files + a collection
-- manifest. Depends only on Assimp (no engine code is LINKED; the one editor include below is a
-- header-only constant table). The launcher will invoke it; runnable by hand too.
--
-- ASSIMP IS LINKED AS A PROJECT AND THERE IS NO DLL TO COPY. This file used to carry, per
-- configuration, a `links { "assimp-vc142-mtd" }` naming the MSVC toolset and a postbuild
-- `if exist ... {COPYFILE}` for a DLL that was NEVER COMMITTED — so the copy silently did nothing on
-- every Windows build, and `if exist` is what made the silence possible. Static now: nothing to copy,
-- nothing to be missing, and Desert/Tests/Editor/AssimpBoundary asserts no build file names a toolset.
project "FbxMeshSplitter"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "**.cpp",
        "**.hpp",
        -- The JSON facade, compiled in as one source (the DesertCtl recipe): collection.json is written through
        -- Editor/Panels/Collections/CollectionManifest.hpp, the header the Collections panel reads it with.
        -- Linking Common instead would drag Optick and Cocoa into a CLI.
        "%{wks.location}/Desert/Common/Source/Common/Json/Json.cpp",
    }

    -- Header-only: Editor/Import/TextureSourceFormats.hpp, the single texture-source priority list this
    -- tool shares with AssimpImporter. Nothing from Editor is compiled or linked here.
    includedirs {
        "%{wks.location}/Editor/Source",
        "%{wks.location}/Desert/Common/Source",
    }

    -- ResultStr needs fmt, header-only from the vendored spdlog.
    defines { "FMT_HEADER_ONLY" }

    -- Assimp: the same pinned, source-built static library the Editor links, on every platform.
    -- BEFORE the configuration filters, and that is not cosmetic: the first version of this block sat
    -- after `filter "configurations:Release"`, so it applied to RELEASE ONLY and the Debug build failed
    -- with "assimp/Importer.hpp file not found". A filter stays in force until the next one.
    externalincludedirs {
        "%{wks.location}/Editor/ThirdParty/assimp/include",
        "%{wks.location}/build/generated/assimp/include",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include",
        "%{wks.location}/ThirdParty/spdlog/include",
    }
    links { "Assimp" }

    filter "system:not windows"
        links { "ReflectCpp" }
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter {}
