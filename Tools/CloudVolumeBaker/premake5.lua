-- CloudVolumeBaker — standalone CLI that bakes a sculpted cloud body into a `.dcmv`.
--
-- It compiles ONE engine source, Engine/Assets/CloudModellingVolume.cpp, rather than linking libDesert:
-- the container and the generator were written GPU-free and dependency-free on purpose, and a baker that
-- needed Vulkan to run could not be run on a build machine. That property is worth keeping, and listing
-- the file here is what checks it — the day the generator reaches for a renderer type, this project stops
-- building.
--
-- Included AFTER Desert/ in the root premake5.lua, unlike the other tools, because it uses the `deps`
-- table that Desert/premake5.lua defines (glm, spdlog and Optick's defines, all of which the container's
-- Result type and the recipe's vectors need).
project "CloudVolumeBaker"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingCatalogue.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
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
