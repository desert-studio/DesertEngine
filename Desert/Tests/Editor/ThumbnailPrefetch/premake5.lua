-- "A cached thumbnail is decoded on a worker before it is drawn; nothing sweeps the project for captures."
--
-- ONE editor translation unit is compiled: ThumbnailPrefetch.cpp, which is free of ThumbnailService and of
-- the device on purpose, so this suite can drive the decode store without a Vulkan instance.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Widgets/ThumbnailPrefetch.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Widgets/ThumbnailEncode.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Widgets/ThumbnailFoliage.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/Serialization/FoliageType.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_image.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        -- CookPaths.hpp takes the cooked-texture path formula from Engine/Assets/CookedTexturePath.hpp
        -- (header-only: Common + std), so the engine's source root is on the path too.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
        deps.DesertSpecific.IncludeDir.stb,
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

    links { "Common", "Optick" } -- FileSystem/VFS live in Common; Common's JobSystem registers with Optick

    filter {}

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
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
