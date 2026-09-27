-- Г21: the gate over "a compute pipeline cannot be built from a shader that did not compile".
--
-- It needs NO GPU, and that is the whole point of where the rule lives. ComputePipeline::Create cannot
-- be linked without the Vulkan backend behind it, so a gate written over Create itself could never run
-- on this machine — and this is precisely the refusal that has to be provable, because for the life of
-- the engine it did not exist and the process died instead. The DECISION therefore sits in
-- Engine/Graphic/Pipeline.hpp as CheckComputePipelineSpecification, which is header-only and device-free,
-- and this suite calls it directly. The second half of the suite reads the sources and pins that Create
-- obeys the rule and that no call site can ignore its answer.
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
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    -- Vulkan headers: Engine/Graphic/Pipeline.hpp reaches the engine's image/framebuffer headers, which
    -- name Vulkan types. Headers only — nothing from the backend is linked.
    for name, path in pairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    -- Engine headers reach DESERT_DEBUG_BREAK, whose expansion is per-platform (Common/Core/Core.hpp).
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    links { "Common", "Optick" } -- Commons JobSystem registers worker threads with Optick

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
