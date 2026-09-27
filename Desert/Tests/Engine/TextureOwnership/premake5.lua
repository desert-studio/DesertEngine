-- "A Texture2D owns its image": dropping the last reference releases the ImageService slot and the image.
--
-- Links the real Texture2D and ImageService and nothing with a device in it: Image2D::Create and the
-- cooked-texture readers are stubbed in the test file, so what is measured is exactly the registration and
-- its release, which is the whole claim.

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The two files the ownership lives in. The GPU half is the one stubbed: Image2D::Create returns a
        -- counted fake, and the registry getter hands out this suite's own ImageService.
        "%{wks.location}/Desert/Desert/Source/Engine/Graphic/Texture.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Runtime/Services/Image/ImageService.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- the walk takes an entt registry
        "%{wks.location}/ThirdParty/reflect-cpp/include",  -- Components.hpp -> ReflectionTypes.hpp -> rfl
        "%{wks.location}/ThirdParty/stb/include",          -- Engine/Text -> stb_truetype
        "%{wks.location}/ThirdParty/JoltPhysics",          -- Components.hpp -> PhysicsWorld -> Jolt
        "%{wks.location}/ThirdParty/lua",                  -- ... -> ScriptProperty -> sol2 -> lua
        "%{wks.location}/ThirdParty/sol2/include",
        "%{wks.location}/ThirdParty/meshoptimizer/src",    -- ... -> Geometry/Mesh
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

    -- Components.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    links { "Common", "Optick" }

    -- Texture.cpp names a texture by Common::Utils::FileSystem::GetFileName, and that TU in libCommon is
    -- the macOS one, so the ObjC runtime and AppKit come with it.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

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
