local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool as a PREBUILD STEP OF `Desert`. Without this edge
    -- a parallel build can compile a stale table in and the assertions would report on yesterday's struct.
    dependson { "Desert" }

    -- WHAT THIS SUITE COMPILES AND WHY THERE IS NO MIGRATION IN THE LIST. The painted layout ADDS fields
    -- and renames none, so there is no v6 -> v7 function: an absent key is already how the reflected
    -- serializer spells "keep the C++ default", and a migration that returned zeros would be the stub §1.2
    -- of the contract forbids. What has to be proved instead is the CONSEQUENCE of that -- that a scene
    -- written before this phase reads back as a layer with no painting -- and proving it needs the
    -- reflection table, the deserializer, and the bake's own parameters to check the result against.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- Components.hpp is an entt registry away
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the scene tree is rfl::Generic
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

    -- Common: UUID/AssetHandle/the logger the migration's warnings go through.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- CloudProceduralVolume.cpp keys its bake in the DDC, whose file store is Common's FileSystem, which
    -- carries Objective-C (MacOSFileSystem's dialogs): the ObjC runtime + AppKit link too.
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
