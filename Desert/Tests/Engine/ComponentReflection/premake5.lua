local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool, which runs as a PREBUILD STEP OF `Desert` — so
    -- without this the parallel build can compile the table for this test before the codegen has rewritten
    -- it, and the test then reports on a stale component set. Build-order only; nothing is linked from it.
    dependson { "Desert" }

    -- The GENERATED reflection table is the thing under test, so it is compiled straight into the test
    -- rather than linked from the engine: the registrations are static initializers in an anonymous
    -- namespace, and Reflection.gen.cpp + the registry are the only two translation units they need.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        -- The cloud packer takes a cloud TYPE's twelve numbers as an argument now, and the payload tests
        -- drive it with the built-in default: the shape an empty slot resolves to lives here.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
        -- THE WRITER AND THE READER OF A `.desce`'s COMPONENT BLOCK, because a census of what a component
        -- EXPOSES is only half of §1.3 if nothing checks that the exposed value comes back. У13 shipped
        -- five fields that were authored in Details and silently never saved, and no suite in this tree
        -- round-trips a real component through this path — ReflectionSerializer next door builds its
        -- TypeInfo by hand, so it proves the mechanism and never the table. It brings nothing with it: no
        -- GPU, no asset layer, no filesystem.
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/", -- Components.hpp is an entt registry away
        "%{wks.location}/ThirdParty/reflect-cpp/include", -- ReflectionTypes.hpp -> <rflcpp/rfl/Generic.hpp>
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

    -- Components.hpp reaches engine headers that use DESERT_DEBUG_BREAK, which needs to know the platform.
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
