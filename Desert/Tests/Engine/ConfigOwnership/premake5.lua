local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- Reflection.gen.cpp is written by DesertHeaderTool, which runs as a PREBUILD STEP OF `Desert`. The
    -- Settings half of the .desce census enumerates that table, so without this the parallel build can
    -- compile a stale copy and audit yesterday's field set. Build-order only; nothing is linked from it.
    dependson { "Desert" }

    -- Nothing is linked from the engine or the Editor: the field lists come from rfl::fields<> over the
    -- HEADERS (which is the same call that writes the files), and the consumer half reads sources as TEXT.
    -- That is what lets one suite hold the editor's preferences, the launcher's descriptor and 81 scene
    -- files side by side without a GPU, a window or an Editor link.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        -- K3: the settings block is written through this file, so the assertion that a machine-quality
        -- change moves none of its bytes has to go through it too rather than through a stand-in.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",              -- Editor/Core/EditorPreferences.hpp: the editor.json struct
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",         -- Components.hpp is an entt registry away
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",   -- rfl::fields<> and rfl::Generic
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

    -- Engine headers reached through Components.hpp use DESERT_DEBUG_BREAK, which needs the platform.
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

    -- K3's machine store is in Common and reads/writes its file through Common::Utils::FileSystem, whose
    -- macOS implementation is Objective-C — so the ObjC runtime + AppKit have to link as well. The same
    -- two lines every other suite that touches FileSystem carries.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
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
