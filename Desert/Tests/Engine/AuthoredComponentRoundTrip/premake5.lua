-- The five components that are authored by hand and have no reflected data block must survive a save.
--
-- Foliage, Locomotion, Morph, SocketAttachment and Projectile each had a full Details editor and no
-- line in ComponentRegistry.cpp until U13, so everything set on them was discarded on the next load.
-- ComponentPersistence next door stops a sixth existing; this suite asserts the other half, which a
-- name in a table cannot: that the mapping is complete and exact, field by field, through JSON TEXT.
--
-- NOTHING FROM THE RENDERER IS LINKED. AuthoredComponentIO.hpp is a pure header for exactly this
-- reason -- ComponentRegistry.cpp itself pulls in the AssetManager and through it the whole engine,
-- so a serializer written inside it can never be round-tripped by anything.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",       -- the component headers are ECS headers
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the block is written and read as JSON text
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

    -- Common: a UUID is Common::UUID and the IO header logs its refusals through Common's logger.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
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
