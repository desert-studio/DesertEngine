local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- FO-3: the foliage brush (UE AddInstancesForBrush) is a pure function of the type, the dab and a traced
    -- world, so the suite drives it with planes (and one landscape) instead of a scene.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/FoliageType.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Panels/ViewportPanel/Tools/FoliageBrush.cpp",
        -- FO-8: the realization rule (pure half; the Scene half is FoliagePrefabsScene.cpp) and the FO-6 filing.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Foliage/FoliagePrefabs.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Foliage/FoliageCells.cpp",
        -- FO-3b: a stroke over a Terrain_Grass-sized landscape through the engine's own landscape ray.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/World/Landscape/LandscapeRaycast.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
        -- LandscapeData.cpp compiles Shaders/Common/LandscapeHeight.glslh as C++.
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",
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

    links { "Common", "Optick" }

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
