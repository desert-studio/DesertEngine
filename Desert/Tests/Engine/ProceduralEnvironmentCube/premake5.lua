-- "The procedural sky's radiance cube is a bake intermediate, and nothing may read it back."
--
-- Two of the three subjects are relations between source files and are READ at run time -- a consumer
-- of Environment::RadianceMap lives in the editor, the producer lives in the engine, and no header
-- connects them, so the only way to state the relation is over the sources.
--
-- The third subject is run rather than read, and it is why this script names ONE engine source:
-- Runtime::ImageService is where "an empty handle names nothing" is decided, and it is pure CPU --
-- a HandlePool, a vector and a generation check. Compiling that one file is much cheaper than linking
-- libDesert, and it keeps the suite device-free. Vulkan/glm include paths are on the list because
-- Engine/Graphic/Image.hpp reaches the graphics headers transitively; nothing from them is linked.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Runtime/Services/Image/ImageService.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.Common.Defines) do
        defines { define }
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

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

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
