local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"
    
    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")
    
    files { 
        test_files,
    }
    
    includedirs {
        "%{wks.location}/Desert/Common/Source",
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

    links { "Common", "Optick" } -- Common drags Optick in through its JobSystem

    -- Common contains Objective-C (MacOSFileSystem file dialog), so any binary that links Common
    -- must bring the ObjC runtime + AppKit along.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

    -- gtest comes from Dependencies.lua (prebuilt .lib on Windows, Homebrew on macOS)
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