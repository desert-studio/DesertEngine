deps = include('Dependencies.lua')
-- dofile, not include: include() runs a file once and returns nil on repeat,
-- and Desert/Dependencies.lua was already included by the engine projects.
local engineDeps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "Editor"
    kind "ConsoleApp"

    files { 
        -- Engine 
        "Source/**.cpp", 
        "Source/**.hpp",
        "ThirdParty/ImGuizmo/ImGuizmo.cpp",

        -- THE SCENE MIGRATOR'S OWN SOURCES, linked into the Editor and into nothing else.
        --
        -- The Editor is the only process that writes `Scenes/Autosave/`, and it is therefore the only
        -- one that can convert it: the directory is gitignored, so it exists on the owner's machine and
        -- in no worktree where a schema step is written, and a task that raises the schema cannot reach
        -- it. CrashRecovery::MigrateAutosaves calls Migration::RunSceneMigrator over that one directory
        -- at startup. `main.cpp` is deliberately NOT listed -- it defines the tool's main().
        --
        -- This does NOT put the old formats back into the engine. `Desert` and `Runtime` link none of
        -- this; the engine's loader still refuses a scene that is not at Core::kSceneVersion, and a
        -- packaged game never opens an autosave.
        "%{wks.location}/Tools/SceneMigrator/Source/SceneMigration.cpp",
        "%{wks.location}/Tools/SceneMigrator/Source/MigratorMain.cpp",
        "%{wks.location}/Tools/SceneMigrator/Source/SettingsCanonical.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Desert/Source/",
        "%{wks.location}/Editor/Source/",

        "%{wks.location}/Desert/Common/Source/",

        -- <MigratorMain.hpp> / the migrator's own includes of its siblings (see the files list above).
        "%{wks.location}/Tools/SceneMigrator/Source",
    }
    externalincludedirs {

        "%{wks.location}/ThirdParty/spdlog/include/",
        "%{wks.location}/ThirdParty/GLFW/include/",
        "%{wks.location}/ThirdParty/Glad/include/",
        "%{wks.location}/ThirdParty/entt/include/",
        "%{wks.location}/ThirdParty/ImGui/",
        "%{wks.location}/ThirdParty/glm/",
        "%{wks.location}/ThirdParty/optick/src/",
        "%{wks.location}/ThirdParty/",
    }

    for name, path in pairs(deps.EditorSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    -- THE VULKAN HEADERS, FROM THE ENGINE'S OWN DISCOVERY, ON EVERY PLATFORM. The Editor includes
    -- Engine/Graphic/API/Vulkan/VulkanDevice.hpp, which opens with <vulkan/vulkan.h>, so it needs the
    -- SDK's include directory exactly as much as the engine does — and EditorSpecific.IncludeDir has
    -- never carried one. On macOS this went unnoticed for as long as the project has existed, because
    -- Homebrew's prefix is added workspace-wide in PlatformMacOS.lua and the header is found by
    -- accident. Windows has no such ambient path, so Editor.vcxproj failed at VulkanDevice.hpp(5)
    -- with C1083 — the Editor was simply not buildable there.
    --
    -- Taken from DesertSpecific rather than copied into EditorSpecific so the SDK is discovered ONCE
    -- (Desert/Dependencies.lua findVulkanSDK) and both consumers name the same directory; a second
    -- copy is a second thing to keep in step. The keys are absent, not empty, when no SDK is found,
    -- so pairs() simply yields nothing and the failure stays where it already is — in the engine.
    for _, key in ipairs({ "Vulkan", "shaderc", "spirv_cross" }) do
        local path = engineDeps.DesertSpecific.IncludeDir[key]
        if path then
            externalincludedirs { path }
        end
    end

    -- NOTE: no INCLUDE_HEADERS=#include<...> define here — nothing uses it, and
    -- the '#' turns the rest of the DEFINES line into a comment in gmake makefiles.
    defines { "YAML_CPP_STATIC_DEFINE",
              "USE_OPTICK=1", "OPTICK_ENABLE_GPU=0", "OPTICK_ENABLE_TRACING=0" }

    links{
        "Desert",
        "yaml-cpp",
        "GLFW",
        "Optick",
        "MeshOptimizer",
        "ImGuiNodeEditor",
    }

    -- Optional: real face tracking via dlib (davisking/dlib). Auto-enabled when the sources are present
    -- at ThirdParty/dlib (clone it there). Absent => Editor/Widgets/FaceTracker compiles as a no-op stub,
    -- so CI and fresh checkouts build without it.
    if os.isdir( _MAIN_SCRIPT_DIR .. "/ThirdParty/dlib" ) then
        defines     { "DESERT_WITH_DLIB" }
        externalincludedirs { "%{wks.location}/ThirdParty/dlib/" }
        links       { "Dlib" }
    end

    filter "configurations:Debug"
    for name, path in pairs(deps.EditorSpecific.Libraries.Debug) do
        links { path }
    end

    filter "configurations:Release"
    for name, path in pairs(deps.EditorSpecific.Libraries.Release) do
        links { path }
    end

    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
        -- EditorLayer.cpp aggregates the Details rendering for every reflected component; the sky's
        -- physical-atmosphere fields pushed its Debug object file past COFF's 65k-section limit
        -- (error C1128). /bigobj lifts the format cap and costs nothing at runtime.
        buildoptions { "/bigobj" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

        -- Unlike Visual Studio, gmake does not link static-lib dependencies
        -- transitively — the executable has to pull in everything the engine
        -- libraries use, plus the Apple frameworks GLFW/MoltenVK rely on.
        links {
            "Common",
            "ImGui",
            "Jolt",
            "Lua",
            "ReflectCpp",
            "Cocoa.framework",
            "IOKit.framework",
            "CoreFoundation.framework",
            "CoreVideo.framework",
            "CoreMedia.framework",
            "AVFoundation.framework",
            "QuartzCore.framework",
        }

    -- Vulkan/shaderc/spirv-cross and reflect-cpp come from the engine's
    -- dependency list so the two stay in sync.
    filter { "system:macosx", "configurations:Debug" }
        for name, path in pairs(engineDeps.DesertSpecific.Libraries.Debug) do
            links { path }
        end

    filter { "system:macosx", "configurations:Release" }
        for name, path in pairs(engineDeps.DesertSpecific.Libraries.Release) do
            links { path }
        end

    filter {}
