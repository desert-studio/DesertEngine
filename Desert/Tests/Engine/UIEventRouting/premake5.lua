-- The ROUTE a pointer event takes through the element tree, asserted between a parent and a child rather
-- than on either of them. Same seam as the UICanvasContext suite next door -- RenderCanvas2D takes a plain
-- entt::registry, a DrawList2D and a UIInput, so a pointer can be synthesised and the messages the canvas
-- fired read back out of the returned vector -- and the same stubbed resource services, for the same
-- reason: every accessor returns nullptr and every draw helper already handles an absent service.
--
-- It is a suite of its own and not more cases in UICanvasContext because the subject is different: that
-- one is about two VIEWS not reaching each other's state, this one is about one view's event reaching the
-- right elements in the right order.
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
        "%{wks.location}/Desert/Desert/Source/Engine/UI/UICanvasLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/UI/UIDataStore.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Text/Utf8.cpp",
        -- The two new fields must survive a reload, and this suite proves it against the SAME reflection
        -- data the scene serializer uses (ComponentRegistry's MakeReflected calls straight through to
        -- SerializeReflected / DeserializeReflected). Compiled in rather than linked because none of the
        -- three needs a GPU or an asset manager.
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp",
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
