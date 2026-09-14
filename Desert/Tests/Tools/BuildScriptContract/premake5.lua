local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The same two translation units Tools/AssetClosure compiles, and for the same reason: this
        -- suite asserts that the closure the PACKAGER ships covers the base scene, so it has to
        -- compute it with the packager's own code rather than with a second implementation.
        -- AssetReferencesScanProject.cpp is deliberately absent — it is the half that needs an engine.
        "%{wks.location}/Editor/Source/Editor/Core/AssetReferences.cpp",
        "%{wks.location}/Editor/Source/Editor/Core/AssetReferencesScan.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Editor/Source",
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

    -- Common carries Objective-C (the macOS file dialog) and the linker pulls whole objects.
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
