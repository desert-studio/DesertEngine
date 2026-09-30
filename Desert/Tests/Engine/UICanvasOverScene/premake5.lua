-- "The canvas is drawn OVER the 3D, it answers the pointer, and neither stops being true when the view
-- changes" (Ю18).
--
-- WHY THIS SUITE AND NOT A FRAME. Half of the property is a position in a Vulkan command buffer, which no
-- test on this machine can observe and which a frame observes only indirectly. That half is expressed as
-- a REGISTER instead -- RenderPhase::k_DeferredOverlayPhases -- and asserted here on plain integers, with
-- the graph's own sort (Engine/Graphic/RenderGraphSort.hpp, header-only) for the ordering it depends on.
-- The other half -- the canvas walk over a registry that also holds a camera, a light and meshes, and the
-- pointer election surviving the view being pointed at another scene and back -- is pure C++ and is
-- asserted directly.
--
-- The frame half of the witness lives in scripts/MacOS/UIOverSceneWitness.sh, which drives a live editor
-- A->B->A through the control channel and finds the canvas's marker panel in the picture. It cannot run
-- in CI; this can.
--
-- Stubs: identical to Desert/Tests/Engine/UIListView -- the renderer resolves sprites, fonts, icons and
-- video through Runtime::ResourceRegistry, whose services own GPU objects, and every draw helper already
-- copes with the service being absent.
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UICanvasRenderer2D.cpp",
        -- ANIM-I9: the walk folds in the frame's UI clips, which are Timeline sequences stepped and evaluated
        -- by UIAnimationPlayback (the UIAnimation host) — so the timeline core comes with it.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UIAnimationPlayback.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Channel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Player.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Binding.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Track.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Sequence.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/SequenceFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Timeline/Evaluator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        -- The frame boundary calls the overlay state machine (Ю12), so the walk is these two files.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UIOverlay.cpp",
        -- Ю15: every authored label the walk draws goes through Localization::Resolve, so the resolver
        -- and the locale table come with it. They pull in no renderer and no device, which is the point.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocalizationService.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocalizedText.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/LocaleFormat.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/StringTable.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Localization/PluralRules.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UIIntrospection.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UICanvasLayout.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UIDataStore.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/UI/UICollectionClone.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Text/Utf8.cpp",
    }

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
