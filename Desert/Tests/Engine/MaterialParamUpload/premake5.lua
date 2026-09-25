-- "A material parameter must reach every (view x frame in flight) copy of its uniform buffer, by
-- whichever route the material was submitted, and one view's write must never reach another's copy."
--
-- Same recipe as Tests/Engine/DescriptorFallbacks and Tests/Engine/GpuTimestampLayout: the units under
-- test hold no VkDevice, so they need no GPU. UniformBufferProperty and FieldProperty are header-only,
-- ShaderResources::UniformBuffer is abstract (the test derives a recording buffer from it, on the
-- production ViewCopiedBlock), and BufferCopyLayout.hpp is pure integer arithmetic. FrameManager and EngineContext are plain counter
-- singletons the test drives by hand to simulate the (frame x slot) matrix.
--
-- Only ViewResources.cpp is compiled in; every other unit is a header. Writing the descriptors themselves does need a device
-- and is therefore NOT covered here.
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
        -- The per-view copy register the uniform-buffer copies live in (GPU-free, see its own suite).
        "%{wks.location}/Desert/Desert/Source/Engine/Graphic/ViewResources.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
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

    links { "Common", "Optick" } -- Commons JobSystem registers worker threads with Optick

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
