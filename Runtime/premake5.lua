-- Runtime — the standalone PLAYER: runs a project's scene in Play mode without any editor UI.
-- Same engine link set as the Editor; contains no Editor/ sources.
-- dofile, not include: include() runs a file once and returns nil on repeat, and both dependency
-- lists were already included by the Editor/engine projects.
local deps       = dofile(_MAIN_SCRIPT_DIR .. '/Editor/Dependencies.lua')
local engineDeps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "Runtime"
    kind "ConsoleApp"

    -- Visual Studio / Xcode start the process here (F5): the engine finds Resources/ under the working
    -- directory, and a checkout keeps it in Editor/. Without this VS starts in build/Bin/<cfg> and stops.
    debugdir "%{wks.location}/Editor"

    files {
        "Source/**.cpp",
        "Source/**.hpp",
    }

    includedirs {
        "%{wks.location}/Desert/Desert/Source/",
        "%{wks.location}/Runtime/Source/",

        "%{wks.location}/Desert/Common/Source/",
    }
    externalincludedirs {

        "%{wks.location}/ThirdParty/spdlog/include/",
        "%{wks.location}/ThirdParty/GLFW/include/",
        "%{wks.location}/ThirdParty/Glad/include/",
        "%{wks.location}/ThirdParty/entt/include/",
        "%{wks.location}/ThirdParty/glm/",
        "%{wks.location}/ThirdParty/optick/src/",
        "%{wks.location}/ThirdParty/",
    }

    for name, path in pairs(deps.EditorSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    defines { "YAML_CPP_STATIC_DEFINE",
              "USE_OPTICK=1", "OPTICK_ENABLE_GPU=0", "OPTICK_ENABLE_TRACING=0" }

    links{
        "Desert",
        "yaml-cpp",
        "GLFW",
        "Optick",
        "MeshOptimizer",
    }

    filter "configurations:Debug"
    for name, path in pairs(deps.EditorSpecific.Libraries.Debug) do
        links { path }
    end

    -- `or Shipping`: the shipping build links the SAME third-party flavour Release does. There is no
    -- third set of prebuilt libraries and inventing one would mean pinning Vulkan and reflect-cpp twice.
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

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

        links {
            "Common",
            "Jolt",
            "Lua",
            "ReflectCpp",
            "Cocoa.framework",
            "IOKit.framework",
            "CoreFoundation.framework",
            "CoreVideo.framework",
            "QuartzCore.framework",
        }

    filter { "system:macosx", "configurations:Debug" }
        for name, path in pairs(engineDeps.DesertSpecific.Libraries.Debug) do
            links { path }
        end

    filter { "system:macosx", "configurations:Release or Shipping" }
        for name, path in pairs(engineDeps.DesertSpecific.Libraries.Release) do
            links { path }
        end

    filter {}
