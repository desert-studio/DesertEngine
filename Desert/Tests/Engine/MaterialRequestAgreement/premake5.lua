local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Two units under test and no engine between them: the pure agreement maths
    -- (Engine/Assets/MaterialParamDiff.hpp) and the demo material table it is asked about
    -- (Editor/Core/DemoMaterials.hpp — which is why Editor/Source is on the include path), checked
    -- against the shipped .demat files read straight off disk as JSON. No renderer, no Vulkan, no
    -- AssetManager: the question is whether two lists of named vec4s agree.
    files {
        test_files,
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",        -- reached through the engine asset headers
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
