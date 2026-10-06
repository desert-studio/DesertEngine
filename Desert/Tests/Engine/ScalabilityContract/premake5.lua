-- ScalabilityContract (SCAL1-C0): the capability catalog, the scalability groups and the recommended settings,
-- pinned against the headers before the implementation exists. No GPU: devices are CatalogProbe fixtures.
-- It links once SCAL1's implementation adds the sources below and Common's Settings/Scalability*.cpp;
-- until then the link failure IS the contract's open state.
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
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/API/Vulkan/DeviceCaps.cpp",
        -- the pure catalog builder; its probe half (ProbeCatalog) is not compiled here
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanCapabilityCatalog.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    if deps.DesertSpecific.IncludeDir.Vulkan then
        externalincludedirs { deps.DesertSpecific.IncludeDir.Vulkan }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: Settings/Scalability, RecommendedQuality, CapabilityCatalog (+ the JobSystem's profiler hooks).
    links { "Common", "Optick" }
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
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
