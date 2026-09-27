local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The subject is Tools/ImageDiff/Source/ImageDiffMath.hpp and nothing else: a header with no state
    -- and no I/O, compiled here, exactly as Tools/LatticePeak/Source/LatticePeakMath.hpp is compiled by
    -- CloudPlacementSpectrum. An instrument nobody can break on purpose is an instrument nobody can
    -- trust, and task HV decides whether four of Unreal's history checks are worth porting by reading
    -- this arithmetic's output.
    files {
        test_files,
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/ImageDiff/Source",
    }

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
