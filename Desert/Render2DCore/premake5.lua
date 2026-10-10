-- Render2DCore (module table: BuildScripts/DesertModules.lua, UE's SlateCore): the RECORDED half of 2D drawing --
-- DrawList2D with its clip regions, transforms and retainer effects, and the font/text data a list is filled
-- from (BakedFont, IconLayer, UTF-8). No GPU: the Vulkan executor that plays a list back is Render2D, in the
-- engine. Depends on Common only, so DesertUI and every tool can record 2D geometry without the engine.
project "Render2DCore"
    kind "StaticLib"
    DesertUnity.EnableForProject() -- no-op without --unity (BuildScripts/UnityBuild.lua)

    pchheader "pch.hpp"
    pchsource "Source/pch.cpp"
    forceincludes { "pch.hpp" }

    files {
        "Source/pch.cpp",
        "Source/pch.hpp",
        "Source/Render2DCore/**.cpp",
        "Source/Render2DCore/**.hpp",
    }

    includedirs {
        "Source/",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
    }

    links { "Common" }

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
