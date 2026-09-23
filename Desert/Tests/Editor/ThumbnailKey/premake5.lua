-- "Two spellings of one asset name one thumbnail; two assets never name the same one."
--
-- ThumbnailKey.hpp is header-only and depends on nothing but Common (AssetHandle + the project-path
-- census), so the naming DECISION is testable without a window or a device -- the same shape, and for
-- the same reason, as the ThumbnailFraming project next door. The defect it guards was invisible to
-- every unit test because the rule lived inline in ThumbnailCache::DiskPath, in a translation unit that
-- pulls in Graphic::Image2D and therefore the whole renderer: it could only be observed by launching the
-- editor and reading filenames out of Cooked/Thumbnails.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files { test_files }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Editor/Source", -- <Editor/Widgets/ThumbnailKey.hpp>
        "%{wks.location}/Desert/Desert/Source", -- CookPaths forwards to <Engine/Assets/CookedTexturePath.hpp>
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

    links { "Common", "Optick" } -- Commons JobSystem registers worker threads with Optick

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
