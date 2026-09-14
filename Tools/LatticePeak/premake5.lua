-- LatticePeak — standalone CLI that measures whether a coverage field is on a grid.
--
-- It is the third instrument of the cloud programme, beside ImageStat and LineJump, and it exists because
-- neither of those two can see periodicity: a histogram is blind to arrangement and a row-to-row step is
-- blind to a period longer than a row. A perfect lattice leaves both of them satisfied.
--
-- UNLIKE its two neighbours this one is NOT engine-free, and the reason is the whole point of the tool:
-- the field mode bakes the placement field through the SHIPPED generator and prints the period that
-- generator predicts beside the period it measures. A tool that re-implemented the placement would be
-- measuring its own arithmetic. It is built exactly the way CloudVolumeBaker is built next door — three
-- engine sources compiled directly rather than libDesert linked — which keeps the GPU-free property of
-- the generator honest: the day the placement reaches for a renderer type, this project stops building.
--
-- Included AFTER Desert/ in the root premake5.lua, like CloudVolumeBaker and for the same reason: it uses
-- the `deps` table that Desert/premake5.lua defines.
project "LatticePeak"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",
        -- The painted layout: CloudProceduralVolume.cpp reads it to decide a cell's coverage, so
        -- everything that compiles the bake compiles this too. It brings nothing with it -- no asset
        -- layer, no GPU, no filesystem -- which is the property that makes adding one line enough.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
        -- The default cloud type, so the tool's shape is the SHIPPED congestus digit for digit rather than
        -- a second copy of fourteen numbers living in a tool.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
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

    -- rfl/json.hpp, which CloudTypeData.cpp reads the type library through.
    for name, path in pairs(deps.CommonSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.Common.Defines) do
        defines { define }
    end

    -- Common: the Result type the generator's refusals are carried in.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    -- ReflectCpp: CloudTypeData.cpp reads and writes `.decloudtype` through rfl::json.
    links { "Common", "Optick", "ReflectCpp" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}
