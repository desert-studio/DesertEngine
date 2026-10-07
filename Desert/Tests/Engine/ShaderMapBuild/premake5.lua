-- The shader map build (text -> metadata + SPIR-V of every stage) run on the job system against the same build
-- run one program at a time: SHC1 moved the cold-start compile of the engine's programs onto the workers, and
-- this is what says the answer did not change. No VkDevice: the half under test never touches one.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderMapBuild.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderCompiler.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderCacheKey.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderMapCache.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderSpirvCache.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/Includer/ShaderIncluder.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShadingModels/ShadingModelManifest.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShadingModels/ShadingModelRegistry.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Tests",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    -- Vulkan headers (for the descriptor types), shaderc and spirv-cross.
    for name, path in pairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    -- The platform macro, the same three lines every other engine-linking test carries. DESERT_DEBUG_BREAK
    -- falls back to MSVC's __debugbreak() when none of the three is defined, so ANY engine header that
    -- reaches DESERT_VERIFY fails to compile here without it.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- The engine defines this for its own Debug TUs; the ShaderCacheKey.cpp compiled INTO this test
    -- must see the same value, so the SPIR-V compared here is compiled with the engine's own profile.
    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
    filter {}

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

    filter {}

    filter "configurations:Debug"
        for name, path in pairs(deps.TestSpecific.Libraries.Debug) do
            links { path }
        end
        for name, path in pairs(deps.DesertSpecific.Libraries.Debug) do
            links { path }
        end

    filter "configurations:Release"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end
        for name, path in pairs(deps.DesertSpecific.Libraries.Release) do
            links { path }
        end

    filter {}

print("Configured test project: " .. test_name)
