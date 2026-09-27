-- The (vertex path x pass) table and the relations that make it checkable: every cell names a shader
-- that exists and calls itself that, every path that can be DRAWN can also be SHADOWED, and each path
-- adds exactly one binding of its own to a surface set that is otherwise identical across paths.
--
-- Links MeshVertexPath.cpp, because the table IS the C++ half of every relation here; the material
-- headers are used for their constexpr push offsets only. No VkDevice: shaders are compiled with shaderc
-- and reflected by VulkanShaderReflection.cpp, the same recipe as Tests/Engine/PBRSceneFrame.
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
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
        -- CollectShaderIncludes: the cascade-text relation derives its consumer set by resolving
        -- each shader's include closure with the engine's own resolver rather than listing them.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderCacheKey.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanShaderReflection.cpp",
        -- The C++ half of every relation below: the table itself.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/Materials/Mesh/MeshVertexPath.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
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
