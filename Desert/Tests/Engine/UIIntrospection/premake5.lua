-- The numbers the UI Debugger panel shows, asserted here instead of on screen.
--
-- The suite compiles the REAL walk (UICanvasRenderer2D.cpp) alongside the introspection module, which is
-- what lets the central relation be written as an assertion rather than as a comment: for every element
-- the enumeration calls SKIPPED, hiding that element must leave the walk's draw list byte-identical. A
-- future change to what the walk skips, or to what opens a batch, reddens this rather than quietly making
-- a panel column wrong.
--
-- Stubs: identical to Desert/Tests/Engine/UICanvasContext -- the renderer resolves sprites, fonts, icons
-- and video through Runtime::ResourceRegistry, whose services own GPU objects, and every draw helper
-- already copes with the service being absent.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/UI/UICanvasRenderer2D.cpp",
        -- The frame boundary calls the overlay state machine (Ю12), so the walk is these two files.
        "%{wks.location}/Desert/Desert/Source/Engine/UI/UIOverlay.cpp",
        -- Ю15: every authored label the walk draws goes through Localization::Resolve, so the resolver
        -- and the locale table come with it. They pull in no renderer and no device, which is the point.
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocalizationService.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocalizedText.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/LocaleFormat.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/StringTable.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Localization/PluralRules.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/UI/UIIntrospection.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/UI/UICanvasLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/UI/UIDataStore.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Text/Utf8.cpp",
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
