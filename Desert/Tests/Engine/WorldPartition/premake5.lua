local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Header-only rules (Engine/Core/Serialize/WorldPartitionRules.hpp): the partitioner and the entity
    -- reference register, WITHOUT the Scene the loader normally hangs them off. SceneSerializer.cpp
    -- reaches the renderer through Scene.hpp and no test project can compile it, which is why the rules
    -- live in a header of their own. Nothing to link but Common, and that is the proof they are pure.
    files {
        test_files,
        -- The document merge, because the round trip of a partitioned world currently GOES THROUGH IT:
        -- SerializeToJson builds a fresh SceneSerialized from the live Scene, which has no partition
        -- member yet, so the block survives a save only because a top-level key the writer does not
        -- state is preserved. That is a claim about this file, so this file is compiled and asserted
        -- rather than described. It is pure -- its only includes are its own header and <utility>.
        "%{wks.location}/Desert/Desert/Source/Engine/Core/Serialize/ForeignKeys.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- the record's component payloads are rfl::Generic
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

    -- PrefabData.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs to know the platform.
    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: UUID and AssetHandle. Optick: Common's JobSystem registers its worker threads with it.
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
