local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- NO ASSIMP, AND THAT IS THE POINT OF THIS SUITE. Every assertion here is about the repository's own
    -- TEXT — which translation units include assimp, which build files name a toolset, what the
    -- packaging script claims and copies — so linking the library would only make the suite slower and
    -- would let a reader think the answers came from the build rather than from the sources. The half
    -- that needs the built library is Desert/Tests/Editor/AssimpLibraryPin next door.
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

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: nothing of it is used by the assertions, but gtest's main links against the workspace's
    -- standard set and Optick is what Common's JobSystem registers its threads with.
    links { "Common", "Optick" }

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
