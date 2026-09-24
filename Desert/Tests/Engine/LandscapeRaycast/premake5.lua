-- LS-7 / LS-8: the landscape as a surface. The ray against the bilinear surface (LandscapeRaycast), the
-- Jolt heightfield bodies built from the same samples (LandscapeCollision, PhysicsWorld against the Jolt
-- library), and the relation between the two. A bare registry stands in for the scene: Scene drags the
-- renderer with it, and nothing here needs a GPU.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/World/Landscape/LandscapeRaycast.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/ECS/System/LandscapeCollision.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Physics/PhysicsWorld.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        -- LandscapeData.cpp compiles Shaders/Common/LandscapeHeight.glslh as C++.
        "%{wks.location}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",
        "%{wks.location}/ThirdParty/reflect-cpp/include",  -- Components.hpp -> ReflectionTypes.hpp -> rfl
        "%{wks.location}/ThirdParty/stb/include",
        "%{wks.location}/ThirdParty/JoltPhysics",          -- PhysicsWorld.cpp is compiled here
        "%{wks.location}/ThirdParty/lua",                  -- Components.hpp -> ScriptProperty -> sol2 -> lua
        "%{wks.location}/ThirdParty/sol2/include",
        "%{wks.location}/ThirdParty/meshoptimizer/src",
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

    links { "Common", "Optick", "Jolt" }

    filter "system:not windows"
        links { "ReflectCpp" }
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
