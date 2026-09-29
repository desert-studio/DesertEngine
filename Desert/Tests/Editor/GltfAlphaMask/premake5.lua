local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- SourceAlphaMode.cpp IS the subject, compiled here rather than restated: the rule that turns a
    -- source material's cut-out statement (glTF alphaMode/alphaCutoff, FBX opacity map) into AlphaCutoff.
    -- The suite writes its own tiny glTF into a temporary folder and parses it with the linked assimp.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/Assimp/SourceAlphaMode.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",  -- <Editor/Import/Assimp/SourceAlphaMode.hpp>
    }

    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/Editor/ThirdParty/assimp/include",
        "%{_MAIN_SCRIPT_DIR}/build/generated/assimp/include",
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
