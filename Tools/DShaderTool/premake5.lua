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
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShadingModels/ShadingModelManifest.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShadingModels/ShadingModelRegistry.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/Shared",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    -- ReflectCpp: DShaderParser reads the shader's AF1 envelope through Common::ShaderAssetHeader, whose
    -- JSON header Common's TextAssetHeader parses through rfl::json (reflect-cpp + its bundled yyjson).
    links { "Common", "ReflectCpp" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        -- The shading-model set is read through Common's FileSystem (VFS-aware), and Common's macOS
        -- FileSystem is Objective-C (the file dialog): linking it needs AppKit + the ObjC runtime.
        links { "Cocoa.framework", "Foundation.framework" }

    filter {}
