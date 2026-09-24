-- THE EDITED MESH SURVIVES A SAVE (M4): the StaticMesh block's EditMesh, written to JSON and read back through
-- the scene's own WriteBlock/ReadBlock, compared as a mesh and as the render mesh derived from it.
--
-- The EditMesh sources and its saved form are compiled in; ComponentRegistry.cpp is not (it links the asset
-- manager and the device), so the suite drives the two calls that registry makes around this block.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshSerialization.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }

    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- PrefabData reaches the asset headers
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the block is written and read as JSON text
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

    links { "Common", "Optick" }

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

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
