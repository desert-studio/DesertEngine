-- "How many undo steps is one interaction on the UI timeline, and does the step put the content back."
--
-- The unit under test is Editor/Core/Commands/UIClipEdit.cpp, lifted out of SequencerPanel.cpp for the
-- reason scripts/CI/UnreachedSources.sh keeps naming: that panel is compiled by no suite, so a rule
-- inside it cannot be checked and an edge missed there is missed in silence. What is left in the panel
-- is the one fact only it knows -- which ImGui widget's edges are one interaction.
--
-- It links NO ANIMATION LAYER, and that is the point of the file existing at all: a UIAnimComponent is
-- not an AnimationClip, has no Animator and has no keyer, so this suite needs the component headers and
-- the undo stack and nothing else. libDesert is not linked either -- it pulls in Vulkan, and none of the
-- three types this trip is about touches the renderer.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool as a PREBUILD STEP OF `Desert`. Components.hpp
    -- is reached through UIClipEdit.hpp, so without this the parallel build can compile against a stale
    -- table. Build-order only; nothing is linked.
    dependson { "Desert" }

    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Core/Commands/UIClipEdit.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- the component headers are ECS headers
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the reflected table is written as JSON text
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

    -- The generated table reaches engine headers that use DESERT_DEBUG_BREAK, which needs the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: an AssetHandle is a Common::UUID and the command logs its refusals.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

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
