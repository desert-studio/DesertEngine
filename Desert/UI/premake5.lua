-- DesertUI (module table: BuildScripts/DesertModules.lua, UE's UMG/Slate): the UI framework -- layout, walk,
-- widgets, focus, overlays, rich text, styles and the authored UI*Data arguments. It reads its element tree
-- through UI::IUITree + NodeId and its resources through IUICanvasResources / IUITextSource /
-- IUIAnimationSource; the ECS-backed implementations of all of them are the ENGINE's (Engine/UI/Ecs/).
--
-- What it may depend on is the point of the project and Desert/Tests/Engine/UIFrameworkBoundary pins it: Common,
-- Render2DCore and CoreReflection's header-only macros. No engine header, no entt, no Vulkan, no GLFW, and
-- never the Desert library itself -- the engine links THIS, not the other way round.
--
-- The reflected UI*Data types (UI/Args) are registered by the ENGINE's generated set: Desert/Desert/premake5.lua
-- scans this tree with `--reflect-root`, and Reflection_DesertUI.gen.cpp is compiled into Desert.
project "DesertUI"
    kind "StaticLib"
    DesertUnity.EnableForProject() -- no-op without --unity (BuildScripts/UnityBuild.lua)

    pchheader "pch.hpp"
    pchsource "Source/pch.cpp"
    forceincludes { "pch.hpp" }

    files {
        "Source/pch.cpp",
        "Source/pch.hpp",
        "Source/UI/**.cpp",
        "Source/UI/**.hpp",
    }

    includedirs {
        "Source/",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Render2DCore/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/CoreReflection/Source",
    }

    links { "Render2DCore", "Common" }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.CommonSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.Common.Defines) do
        defines { define }
    end

    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }

    filter { "system:windows" }
        defines { "DESERT_PLATFORM_WINDOWS" }
        -- Same as Desert/Desert/premake5.lua: MSVC caps an object file's sections, and the unity build merges.
        buildoptions { "/bigobj" }

    filter { "system:macosx" }
        defines { "DESERT_PLATFORM_MACOS" }

    filter {}
