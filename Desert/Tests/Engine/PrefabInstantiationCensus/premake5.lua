local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- "THERE IS ONE PLACE A PREFAB INSTANCE IS BUILT, AND ONE PLACE ITS ADDRESS IS SPELLED" -- as a
    -- census over the SOURCE TEXT, plus a sweep of every .deprefab on disk. The text half is read off
    -- disk rather than linked, because what it asserts is the SET OF FILES that name a function, and
    -- that is invisible to anything that links them. The corpus half parses the files with the same
    -- PrefabFormat.cpp the loader uses and classifies their root with the same PrefabPlacement.cpp the
    -- instantiator uses -- both pure, both linkable without a renderer.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Prefab/PrefabPlacement.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",       -- PrefabData reaches ECS headers
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the scene tree is rfl::Generic
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

    -- PrefabData.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs to know the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: UUID and AssetHandle. Optick: Common's JobSystem registers its worker threads with it.
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
