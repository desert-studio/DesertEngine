-- CloudLayoutBaker — standalone CLI that turns a picture into a `.dclayout`.
--
-- WHY A TOOL EXISTS BESIDE THE PANEL. The panel is what the owner asked for — "load a texture" is a thing
-- you do by dropping a file into a slot — and it is where an artist works. This is the other half: a
-- painting has to be reproducible from a command, or the shipped example in Resources/Assets/Clouds/Layouts
-- is a binary nobody can regenerate, and the acceptance frame of this phase rests on a file whose
-- provenance is "somebody clicked". The same division `.dcmv` already has between Tools/CloudVolumeBaker
-- and the sculpting panel.
--
-- It compiles ONE engine source, Engine/Assets/CloudLayout.cpp, rather than linking libDesert — and it does
-- NOT compile CloudLayoutAsset.cpp, which is the asset-layer wrapper and drags in the VFS. The container
-- and its arithmetic were written free of the GPU, the asset system and the filesystem on purpose, and
-- listing exactly one file here is what CHECKS that: the day the encoder reaches for a renderer type or an
-- asset manager, this project stops building.
--
-- Included AFTER Desert/ in the root premake5.lua, like CloudVolumeBaker, because it uses the `deps` table
-- that Desert/premake5.lua defines.
project "CloudLayoutBaker"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{wks.location}/ThirdParty/stb/stb_image.cpp",
    }

    includedirs {
        "%{wks.location}/Tools/Shared",
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/stb/include",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.Common.Defines) do
        defines { define }
    end

    -- Common: the Result type the container's refusals are carried in.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        -- The bake is written through Common::Utils::FileSystem (the tree's one write primitive), and
        -- Common's file dialog is Objective-C, so linking it needs AppKit + the ObjC runtime. Same
        -- reason PakTool carries these two lines.
        links { "Cocoa.framework", "Foundation.framework" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}
