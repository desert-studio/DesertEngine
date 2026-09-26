local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool, which runs as a PREBUILD STEP OF `Desert`. This
    -- suite round-trips the REAL reflected UI components through it, so without this the parallel build
    -- can compile a stale table and certify yesterday's fields. Build-order only; nothing is linked.
    dependson { "Desert" }

    -- The generic serializer and the reflected type table are compiled in directly. libDesert is NOT
    -- linked: it pulls in Vulkan, and everything this trip needs — the field table, the byte-offset
    -- walker and rfl::json — is free of the renderer. That separation is the whole reason a UI
    -- component's round trip can be asserted at all; ComponentRegistry's resolver cannot be, which is
    -- why the resolver is a stub here and the texture half lives in TextureSlotRoundTrip.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",       -- the component headers are ECS headers
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the record is written and read as JSON text
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

    -- Common: an AssetHandle is a Common::UUID and the serializer logs its refusals.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

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
