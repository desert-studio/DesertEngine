local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The subject is WHICH FILES EXIST in the shader tree and WHAT THEIR TEXT READS, which no engine
    -- symbol can answer, so this suite compiles nothing of the engine at all: it reads the tree. The one
    -- engine thing it needs is a HEADER — Graphic::kCloudUnreadSlots, the register of parameter-block
    -- slots no shader reads, which the census below checks the shader text against in both directions.
    files { test_files }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/", -- VolumetricCloudComponent.hpp is an entt registry away
    }

    -- The platform macro: CloudPayload.hpp reaches engine headers that use DESERT_DEBUG_BREAK.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

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
