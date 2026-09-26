local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The subject is the RELATION between the two LAYOUT TEXTURES and everything downstream of them: the
    -- pattern says WHERE the clouds are, the mask says how much to ADD or REMOVE, and since O-4 they are
    -- two pictures, two material inputs and two independently bakeable sources -- Unreal's own arrangement
    -- (Docs/Clouds/RESEARCH_LAYOUT_TEXTURES.md 1.1, 3).
    --
    -- WHY IT IS NOT ONLY A ROUND TRIP. Task O-3 measured the trap directly on the noise sheet: permuting the
    -- tile order in BOTH codecs at once leaves the round trip perfect and the on-disk agreement silently
    -- changed. So the load-bearing assertions here are ABSOLUTE -- which channel of the exported picture is
    -- which species slot, and what byte the mask's neutral is -- stated against literals rather than against
    -- the importer, and the round trip sits beside them rather than standing in for them.
    --
    -- The bake is compiled in because the mask's whole contract is a statement about COVERAGE, not about
    -- pixels: neutral changes nothing, brighter adds, darker removes, and the two inputs are independent.
    -- Those are relations between a table and a baked cell, and neither side alone can carry them.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Tools/LatticePeak/Source",
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

    -- Common: the Result/error type every refusal in the generator is carried in.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- CloudProceduralVolume.cpp keys its bake in the DDC, whose file store is Common's FileSystem, which
    -- carries Objective-C (MacOSFileSystem's dialogs): the ObjC runtime + AppKit link too.
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
