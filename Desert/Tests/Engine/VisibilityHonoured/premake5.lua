local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Two instruments in one suite, and they answer different questions.
    --
    -- The PREDICATE half is pure: Engine/ECS/EntityVisibility.hpp is one inline function over an entt
    -- registry, so those tests build registries by hand and need no Scene, no device, no AssetManager.
    --
    -- The CENSUS half reads the engine's SOURCE TEXT rather than linking it, and that is a deliberate
    -- limitation, not a shortcut: the collectors it polices (SkyboxECSSystem, VolumetricCloudECSSystem,
    -- MeshECSSystem) include SceneRenderer.hpp and therefore the whole Vulkan stack, so "run the system
    -- and inspect the commands it emitted" would mean standing up a device inside a unit test. The
    -- behavioural evidence for those lives in a rendered frame instead; this suite's job is to make a
    -- DELETED check impossible to miss, and a scan that names the system and the walk does that.

    files { test_files }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",        -- Components.hpp is an entt registry away
        "%{wks.location}/ThirdParty/reflect-cpp/include",  -- rfl::Generic and rfl::fields<>
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

    -- Engine headers reached through Components.hpp use DESERT_DEBUG_BREAK, which needs the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

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
