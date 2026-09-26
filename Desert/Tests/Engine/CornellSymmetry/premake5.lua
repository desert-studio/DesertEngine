local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Two units under test and no engine between them: the shipped shader maths
    -- (Editor/Resources/Shaders/Mesh/{PBRFunctions,DirectLighting}.glslh, driven AS C++ through
    -- CornellSymmetryReference.hpp — which is why the SHADER ROOT is on the include path), and the
    -- shipped ASSETS (Editor/Resources/Assets/{Scenes/CornellDemo.desce,Materials/CB_*.demat}), read
    -- straight off disk as JSON. The relation the suite asserts holds between those two and nothing
    -- else, so there is no renderer, no Vulkan and no AssetManager here.
    files {
        test_files,
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include",  -- rfl, under Common::Json
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

    -- Common: the assets are read through Common::Json (Parse/Node, Read), which lives in the library.
    -- Optick: Common's JobSystem registers its worker threads with the profiler; Cocoa/Foundation:
    -- Common's FileSystem carries Objective-C (MacOSFileSystem's dialogs).
    links { "Common", "Optick" }
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
