-- DShaderTool — standalone CLI over the ENGINE's Desert Shader Language parser (single source of
-- truth: DShaderParser.cpp is compiled in directly, same recipe as its unit test). Lints .shader
-- files offline — used by CI so a broken shader fails the pipeline instead of the editor at runtime.
-- dofile, not include: the dependency list was already include()'d by the engine projects.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "DShaderTool"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        -- The parser depends only on Core/Formats headers + Common — no engine lib needed.
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
    }

    includedirs {
        "%{wks.location}/Tools/Shared",
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    links { "Common" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

    filter {}
