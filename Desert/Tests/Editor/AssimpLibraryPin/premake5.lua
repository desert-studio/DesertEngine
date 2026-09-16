local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- THIS SUITE LINKS THE BUILT LIBRARY ON PURPOSE. Its three questions — which formats did we get,
    -- which version did we get, and what does a real file import to — can only be answered by the
    -- artifact, not by the build files that asked for it. The text half is AssimpBoundary next door.
    files {
        test_files,
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
    }

    externalincludedirs {
        "%{wks.location}/Editor/ThirdParty/assimp/include",
        "%{wks.location}/build/generated/assimp/include",
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

    -- Common: nothing of it is used by the assertions, but gtest's main links against the workspace's
    -- standard set and Optick is what Common's JobSystem registers its threads with.
    links { "Common", "Optick", "Assimp" }

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
