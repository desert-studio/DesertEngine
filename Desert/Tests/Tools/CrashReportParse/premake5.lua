local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

-- CR1c: the crash reporter's crash.txt reader, compiled from its own source. The reporter links no engine
-- (Tools/CrashReporter/premake5.lua says why), and CrashReport.cpp needs nothing but the standard library,
-- so this suite links only gtest: it reads the report exactly as the window that shows it does.
project(test_name)
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Tools/CrashReporter/Source/CrashReport.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/CrashReporter/Source",
    }

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

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
