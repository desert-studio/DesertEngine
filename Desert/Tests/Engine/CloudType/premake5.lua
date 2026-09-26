local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- One unit under test — the `.decloudtype` format — and one body of CONTENT: the nine shipped presets,
    -- which this suite opens off the disk rather than embedding. Embedding them would be a third statement
    -- of the same numbers, and it would pass while the shipped files were broken.
    --
    -- The engine source is listed rather than linked because libDesert pulls in Vulkan and the whole
    -- renderer; the format needs neither, which is exactly why it was put where it could be tested without
    -- a device. The profile generator it includes is a header.
    -- CloudNoiseVolume.cpp is listed for one reason: a type NAMES a noise volume, and since the coverage
    -- field moved onto the billowy pair that volume's lattice decides the type's placement cells as well
    -- as its edge. The relation test therefore reads the recipe out of the shipped `.dcnv` through the
    -- shipped decoder rather than out of a table here — the recipe is in the file, which is the whole
    -- claim the format makes.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
        -- The PRODUCER, because "what this type puts in the sky" is what it places and no longer what a
        -- curve in a header said it would. The anchor is the shipped library either way; what moved is
        -- the instrument it is measured with.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",
        -- The painted layout: CloudProceduralVolume.cpp reads it to decide a cell's coverage, so
        -- everything that compiles the bake compiles this too. It brings nothing with it -- no asset
        -- layer, no GPU, no filesystem -- which is the property that makes adding one line enough.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
    }

    -- The SHADER ROOT is here so that Common/CloudGeometry.glslh — the march's step schedule — can be
    -- compiled as C++ beside the library it has to agree with (CloudScheduleReference.hpp).
    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the file is rfl::json
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

    -- Common: the Result/error type every refusal is carried in.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- CloudProceduralVolume.cpp keys its bake in the DDC, whose file store is Common's FileSystem, which
    -- carries Objective-C (MacOSFileSystem's dialogs): the ObjC runtime + AppKit link too.
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
