local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Two units under test, both GPU-free:
    --   * Engine/Content/Shaders/Common/SkyMedium.glslh — the physical atmosphere's medium sampling,
    --     phase functions, ray/sphere distances, LUT parameterisations and the transmittance integrator
    --     (Hillaire 2020) — compiled AS C++ through SkyMediumReference.hpp. That is why the SHADER ROOT
    --     is on the include path: the test drives the exact text both LUT compute passes compile.
    --   * Engine/Graphic/SkyPayload.hpp — the packed medium block and its unit conventions. Header-only,
    --     so it links without the engine.
    -- Nothing to link — no renderer, no Vulkan.
    files {
        test_files,
        -- The ENGINE's own CPU evaluation of the ground transmittance, compiled INTO this test rather
        -- than mirrored by it: SkyGroundTransmittance.cpp is what reddens the directional light, it
        -- compiles the same SkyMedium.glslh, and it depends on nothing but glm and header-only payload
        -- structs — so the suite that owns the maths can assert on the function that ships, not on a
        -- listing that looks like it.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/SkyGroundTransmittance.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Engine/Content/Shaders",
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
