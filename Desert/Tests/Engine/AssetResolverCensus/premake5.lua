local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool, which runs as a PREBUILD STEP OF `Desert`. This
    -- suite enumerates SceneSettings' reflected field list, so without this the parallel build can compile
    -- a stale copy and certify yesterday's fields. Build-order only; nothing is linked from it.
    dependson { "Desert" }

    -- Nothing else is linked from the engine. The struct that owns the debug view, the tool's key list and
    -- the 80-odd scene files are all read as TEXT/JSON, which is what lets one suite hold an engine
    -- header, a header from a different TARGET and the data corpus side by side without a GPU.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        -- The WRITE half of the resolver, so the census can CALL it instead of grepping for it. It was
        -- extracted from ComponentRegistry.cpp precisely so that this is possible, and it reaches
        -- AssetHandle and nothing else, so it costs this suite no renderer.
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/StoredAssetForm.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",       -- the generated table reaches ECS headers
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- scene files are parsed as rfl::Generic
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

    -- The generated table reaches engine headers that use DESERT_DEBUG_BREAK, which needs the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: the generated table default-constructs an AssetHandle, which is a Common::UUID.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

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
