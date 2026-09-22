-- The engine's depth convention, tested as numbers.
--
-- Three things that must agree and are written in three different files: the projection factory
-- (Engine/Core/Projection.hpp), the frustum's plane extraction (Engine/Core/Frustum.cpp), and the
-- shader-DSL depth-compare mapping (Engine/Graphic/PipelineCache.hpp). Each is individually plausible;
-- only their agreement is correct, and a picture is the only thing that ever showed a disagreement.
--
-- No renderer and no device: ApplyShaderRenderState is an inline function over plain structs, the
-- projections are glm, and Frustum.cpp touches nothing but glm.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Frustum.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        -- The SHADER ROOT, so Common/ViewRay.glslh can be compiled as C++ by ViewRayReference.hpp: the
        -- background-ray tests below drive the exact text the sky passes compile, which is what makes a
        -- pass a statement about the code the GPU runs rather than about a copy of it.
        "%{wks.location}/Editor/Resources/Shaders",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    links { "Common", "Optick" } -- Common's JobSystem registers worker threads with Optick

    -- Common/Core/Core.hpp's DESERT_DEBUG_BREAK needs to know the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    filter "configurations:Debug"
        for name, path in pairs(deps.TestSpecific.Libraries.Debug) do
            links { path }
        end

    filter "configurations:Release"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end

    filter {}

print("Configured test project: " .. test_name)
