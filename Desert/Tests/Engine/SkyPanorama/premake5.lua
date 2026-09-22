-- The HDR sky's panorama lookup, tested as numbers AND as a census over the two programs that use it.
--
-- Two questions, and they fail in different ways. The NUMBERS: a yaw applied to the lookup must turn the
-- IMAGE the other way, or an author typing 90 gets 270 and there is no frame that says so without a
-- protractor. The CENSUS: PanoramaToCubemap writes the cube the sky is drawn from and DiffuseIrradiance
-- integrates the same panorama into the cube that lights every surface -- if the look reaches one and
-- not the other, the visible sky and the ambient it casts describe different skies, which is precisely
-- the state this engine shipped with `Intensity` for its whole life.
--
-- No renderer and no device: glm, and two files read as text.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        -- The SHADER ROOT, so Common/SkyPanorama.glslh compiles as C++ (SkyPanoramaReference.hpp) --
        -- the same text the two bake programs compile as GLSL.
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
