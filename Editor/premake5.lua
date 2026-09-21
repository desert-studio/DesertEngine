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
        -- The PROJECT, not a file: BuildScripts/ThirdParty/Assimp.lua compiles the pinned submodule.
        -- The name it replaced carried the MSVC toolset in it (`assimp-vc142-mtd`).
        "Assimp",
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

    -- `or Shipping`, for the reason written in Desert/Desert/premake5.lua: the shipping build links the
    -- SAME third-party flavour Release does, and inventing a third set would pin Vulkan twice. This set
    -- is EMPTY today (Editor/Dependencies.lua: assimp became a premake project and nothing replaced it),
    -- so this arm moves no bytes -- it is here because an empty set that grows a member tomorrow must
    -- not be the fourth place the shipping configuration is forgotten. The arm that was actually load
    -- bearing is the macOS one further down, and its note is where this defect is written up.
    filter "configurations:Release or Shipping"
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

    -- `or Shipping`, AND ITS ABSENCE IS WHAT THIS BLOCK IS REALLY ABOUT. This is where the Editor gets
    -- Vulkan, shaderc and spirv-cross on macOS -- the comment above says why it needs them by name here
    -- and Visual Studio does not: gmake will not pull a static library's own dependencies through.
    --
    -- The shipping configuration arrived with this arm taught to Runtime/premake5.lua and NOT to this
    -- one. A premake filter that matches no configuration contributes nothing and says nothing, so
    -- `make config=shipping` -- which is what `scripts/MacOS/BuildMacOS.sh Shipping` runs, and what the
    -- packager's own "Runtime binary not found" message sends people to -- compiled the entire tree and
    -- then died at the Editor's link on the whole of Vulkan, the whole of shaderc and the whole of
    -- spirv-cross. MEASURED, not deduced, and nothing in the sources could have pointed here: every
    -- translation unit built. Only the link of the one project this configuration had never been asked
    -- to produce failed. Windows was never affected -- MSVC links project references transitively, so
    -- the Editor picks the same libraries up through Desert.vcxproj, which В9 did teach.
    --
    -- Desert/Tests/Runtime/ShippingBoundary asserts this relation over EVERY such arm in EVERY project
    -- script, so the next configuration-shaped library selection is covered the day it is written.
    filter { "system:macosx", "configurations:Release or Shipping" }
        for name, path in pairs(engineDeps.DesertSpecific.Libraries.Release) do
            links { path }
        end

    filter {}
