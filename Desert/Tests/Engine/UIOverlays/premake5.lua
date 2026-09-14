-- OVERLAYS (Ю12): a tooltip, a context menu, a modal and a toast are one thing four times -- a canvas
-- with a higher Sort Order, drawn last, taking the pointer from what it covers. This suite asserts the
-- RELATIONS that makes true, not the four features one at a time:
--
--   * a press that misses a modal does not reach the element under it, and DOES when the modal is closed;
--   * a tooltip against the right edge flips to the other side of the pointer instead of leaving the view;
--   * a toast leaves on its own clock, the stack is bounded and the queue behind it is bounded too;
--   * a context menu closes on Escape and on a click outside, and a submenu is the same mechanism nested.
--
-- Same seam as the two UI suites next door: RenderCanvas2D takes a plain entt::registry, a DrawList2D and
-- a UIInput, so a pointer and a keystroke are synthesised rather than injected, and the same stubbed
-- resource services, for the same reason -- every accessor returns nullptr and every draw helper already
-- handles an absent service.
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
        "%{wks.location}/Desert/Desert/Source/Engine/UI/UIOverlay.cpp",
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
