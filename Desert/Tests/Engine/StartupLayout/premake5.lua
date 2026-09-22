local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- What the editor works out about itself from its own executable's position: which engine tree
    -- it belongs to, which project to open when nobody named one, and where the engine resources
    -- are. All three are pure functions of paths, which is why this suite needs no window, no
    -- device and no editor -- only Common (for the result type and fmt).
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Project/StartupLayout.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
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

    links { "Common", "Optick" }

    -- Linking Common on macOS brings its Objective-C half (the Cocoa file panels) into the link
    -- even though nothing here opens a dialog; the frameworks are what satisfies the linker.
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
