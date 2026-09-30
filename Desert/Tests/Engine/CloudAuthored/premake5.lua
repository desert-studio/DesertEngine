local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Two units under test, both GPU-free:
    --   * Editor/Resources/Shaders/Common/CloudAuthored.glslh and Common/CloudField.glslh — the authored
    --     producer's addressing, the union and the cutout — compiled AS C++ through
    --     CloudAuthoredReference.hpp, which is why the SHADER ROOT is on the include path.
    --   * Engine/Assets/CloudModellingVolume.cpp — the container and the smooth-min bake. The suite bakes
    --     the SHIPPED recipe rather than a fixture of its own, so "the volume and its reading agree" is a
    --     statement about the file the demo scene actually loads.
    -- The other two engine sources are the ones the seam's PROCEDURAL half needs: the profile table the
    -- march binds and the cloud type it is generated from.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
        -- The procedural producer: the seam calls BOTH, and a stubbed zero on the other side would
        -- leave the union untested because the sculpted body would win everywhere.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",
        -- The painted layout: CloudProceduralVolume.cpp reads it to decide a cell's coverage, so
        -- everything that compiles the bake compiles this too. It brings nothing with it -- no asset
        -- layer, no GPU, no filesystem -- which is the property that makes adding one line enough.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudModellingCatalogue.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the cloud type's file format is rfl::json
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

    -- Common: the Result/error type the container's refusals are carried in, and Constants::Path.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

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
