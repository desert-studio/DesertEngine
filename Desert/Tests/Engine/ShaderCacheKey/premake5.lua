-- The SPIR-V cache key, the include closure it is computed over, and the descriptor count a pipeline
-- layout and a bound descriptor set have to agree on.
--
-- All three run with NO VkDevice, which is the only reason they are checkable on this machine:
-- ShaderCacheKey.cpp is pure file I/O + hashing, and VulkanShaderReflection.cpp holds no device (same
-- recipe as Tests/Engine/ShaderReflection). The engine's own translation units are compiled straight
-- into the test, so what is asserted is the code the runtime executes rather than a copy of it.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderCacheKey.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderMapCache.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanShaderReflection.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
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
    -- must see the same value or SpirvDebugInfoThisBuild() here disagrees with the engine's — and
    -- the profile-relation tests below would pin the wrong mapping.
    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
    filter {}

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

    -- Common contains Objective-C (MacOSFileSystem's file dialog) and the include walk goes through
    -- Common::Utils::FileSystem, so the ObjC runtime + AppKit have to link as well.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
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
