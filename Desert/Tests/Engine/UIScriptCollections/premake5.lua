-- UIL1: the Lua side of the collection a UIListView binds to — ui.list_add / insert / remove / set / clear /
-- count run in a real sol2 state against the real UIDataStore, so a script's record reaches the same
-- collection the list walk reads, with Lua's 1-based indices translated exactly once.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The unit under test is the Lua side of the bridge: the ui.* table and the store it writes.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Scripting/UIBindings.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UIDataStore.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UIOverlay.cpp",
        -- ui.toast reaches UIOverlayRequests, whose file also places overlays through the canvas layout.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UICanvasLayout.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Text/Utf8.cpp",
    }

    links { "Lua" }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/entt/include/",       -- the walk takes an entt registry
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",  -- Components.hpp -> ReflectionTypes.hpp -> rfl
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/include",          -- Engine/Text -> stb_truetype
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/JoltPhysics",          -- Components.hpp -> PhysicsWorld -> Jolt
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/lua",                  -- ... -> ScriptProperty -> sol2 -> lua
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/sol2/include",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/meshoptimizer/src",    -- ... -> Geometry/Mesh
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
