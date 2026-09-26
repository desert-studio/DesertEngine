local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- TWO SUBJECTS, AND THEY ARE TWO HALVES OF ONE CLAIM.
    --
    -- The first is Tools/LatticePeak/Source/LatticePeakMath.hpp, the arithmetic the instrument that found
    -- this phase's defect is made of. An instrument nobody can break on purpose is an instrument nobody can
    -- trust: if the autocorrelation had an off-by-one in its lag then every "the lattice is gone" reported
    -- by it would be that off-by-one. It is a header with no state and no I/O precisely so that it can be
    -- compiled here — the same trick the `.glslh` suites use on the shader maths, arrived at from the other
    -- side.
    --
    -- The second is the PLACEMENT in Engine/Assets/CloudProceduralVolume.cpp, measured with that arithmetic.
    -- CloudModellingVolume.cpp comes with it because the join and the distances are shared rather than
    -- copied.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
        -- The painted layout: the placement reads it, so the placement's suite compiles it. It brings no
        -- asset layer with it by design — CloudLayout.cpp knows about pixels and arithmetic and nothing
        -- about files, which is what lets the tables be built in a test without a disk.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
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
