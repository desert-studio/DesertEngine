-- DesertHeaderTool — reflection codegen. The emitted text lives in Templates/*.tpl, rendered by
-- Common::Text (Common/Json/Template.hpp); the tool links Common alone, like AssetRegistryTool, so it
-- still builds before the engine it generates for (Desert dependson this project).
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "DesertHeaderTool"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "**.cpp",
        "**.hpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/Shared",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
    }

    for name, path in DesertSortedPairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    -- CommonSpecific carries reflect-cpp: the template data model is a Common::Json value tree.
    for name, path in DesertSortedPairs(deps.CommonSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    -- Lua: Source/ModuleTable.cpp executes BuildScripts/DesertModules.lua, the module table premake reads, so
    -- the reflected types are grouped by module from the same text (plan C11). Lua is a ThirdParty project
    -- above Desert/, so this tool still builds before the engine.
    externalincludedirs { "%{_MAIN_SCRIPT_DIR}/ThirdParty/lua" }

    links { "Common", "ReflectCpp", "Lua" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

    filter {}
