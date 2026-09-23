#pragma once

// THE REGISTER: every runtime resource an entity holds outside the registry, and the line that gives it back.
//
// WorldPartition streaming unloads a cell by destroying its entities, so "the entity is gone" has to mean
// every resource it caused to exist is gone too. EnTT frees the components themselves; what it cannot free
// is anything a component or a system holds BY NUMBER — a Jolt body id, a CharacterVirtual slot, a cache row
// keyed by the entity. Until WP6 two of those (Jolt bodies and characters) had no release call anywhere in
// the tree, and one cache (AttachmentSystem's bone refs) was erased by nothing.
//
// The census in runtime_handle_census_test.cpp finds such holders ITSELF, from the source text, and
// requires one row here per holder. A new component field of a handle type, or a new system map keyed by
// entt::entity, is red until someone writes down how it is released — and the row's evidence must be
// present in the file it names, so a row cannot outlive the code it describes.

#include <array>
#include <string_view>

namespace Desert::Tests::RuntimeHandles
{
    // A component FIELD that holds a handle into a system (a number, not an owning pointer). Released by an
    // `on_destroy<Component>` listener that calls `ReleaseCall`; both must appear in `ReleasedIn`.
    struct ComponentHandleRow
    {
        std::string_view Component;
        std::string_view Field;
        std::string_view ReleasedIn; // repo-relative
        std::string_view ReleaseCall;
    };

    inline constexpr std::array<ComponentHandleRow, 2> kComponentHandles{ {
         { "RigidBodyComponent", "RuntimeBody", "Desert/Desert/Source/Engine/ECS/System/PhysicsBodyLifetime.cpp",
           "RemoveBody" },
         { "CharacterControllerComponent", "RuntimeCharacter",
           "Desert/Desert/Source/Engine/ECS/System/PhysicsBodyLifetime.cpp", "RemoveCharacter" },
    } };

    // How a system-side table keyed by entity drops the rows of destroyed entities.
    enum class Release
    {
        Destroyer,    // erased by the destroy path itself (Core::DestroyEntityTree); Evidence = that call
        Listener,     // an on_destroy<Component> listener erases the row; Evidence = its `.connect`
        Sweep,        // a per-frame pass erases rows whose entity is no longer valid; Evidence = the test
        OwnerRetired, // the table lives INSIDE a per-owner object that is itself swept; Evidence = that sweep
        Exception,    // KNOWN not released on entity destroy — see Why; each one is a named, open task
    };

    // A system-side table keyed by an entity. `Member` must still be declared in `File`, and `Evidence` must
    // appear (whitespace-insensitively) in `EvidenceFile` — the same file unless the release lives elsewhere.
    struct EntityTableRow
    {
        std::string_view File; // repo-relative
        std::string_view Member;
        Release          How;
        std::string_view EvidenceFile;
        std::string_view Evidence;
        std::string_view Why;
    };

    inline constexpr std::array<EntityTableRow, 12> kEntityTables{ {
         { "Desert/Desert/Source/Engine/Core/SceneEntityIndex.hpp", "m_SlotOf", Release::Destroyer,
           "Desert/Desert/Source/Engine/Core/SceneEntityIndex.cpp", "index.Remove( *it )",
           "the scene's own entity index; the destroy path removes the row before registry.destroy" },
         { "Desert/Desert/Source/Engine/ECS/System/AudioECSSystem.hpp", "m_Sources", Release::Sweep,
           "Desert/Desert/Source/Engine/ECS/System/AudioECSSystem.hpp", "!registry.valid( it->first )",
           "per-frame sweep destroys the miniaudio source" },
         { "Desert/Desert/Source/Engine/ECS/System/AttachmentSystem.hpp", "m_BoneRefs", Release::Listener,
           "Desert/Desert/Source/Engine/ECS/System/AttachmentSystem.hpp", "on_destroy<SocketAttachmentComponent>().connect",
           "WP6: was erased by nothing" },
         { "Desert/Desert/Source/Engine/ECS/System/AttachmentSystem.hpp", "m_RefRig", Release::Listener,
           "Desert/Desert/Source/Engine/ECS/System/AttachmentSystem.hpp", "on_destroy<SocketAttachmentComponent>().connect",
           "WP6: was erased by nothing" },
         { "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp", "m_Canvases", Release::Sweep,
           "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp", "reg.valid( it->first )",
           "RetireDeadCanvases, once per frame" },
         { "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp", "HoverT", Release::OwnerRetired,
           "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp", "m_Canvases.erase( it )",
           "per-element float inside a canvas's context; goes when the canvas does" },
         { "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp", "TweenT", Release::OwnerRetired,
           "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp", "m_Canvases.erase( it )",
           "per-element float inside a canvas's context; goes when the canvas does" },
         { "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp", "TweenSeen", Release::OwnerRetired,
           "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp", "m_Canvases.erase( it )",
           "per-element counter inside a canvas's context; goes when the canvas does" },
         { "Desert/Desert/Source/Engine/Graphic/Render2D/UIRenderTextureCache.hpp", "m_Captures", Release::Sweep,
           "Desert/Desert/Source/Engine/Graphic/Render2D/UIRenderTextureCache.cpp", "m_Captures.erase( element )",
           "capture not demanded this frame is released after one device idle for the batch" },
         { "Desert/Desert/Source/Engine/Graphic/Render2D/UIRenderTextureCache.hpp", "m_Refused", Release::Sweep,
           "Desert/Desert/Source/Engine/Graphic/Render2D/UIRenderTextureCache.cpp", "m_Refused.erase( element )",
           "one log line per refused element, forgotten when the element is demanded again" },
         // Keyed by `uint32_t` (the entity's integer value), so the text scan for entt::entity keys cannot see
         // these two; they are listed by hand and their declarations are still checked to exist.
         { "Desert/Desert/Source/Engine/Scripting/Internal/ScriptRuntime.hpp", "Envs", Release::Listener,
           "Desert/Desert/Source/Engine/ECS/System/ScriptSystem.hpp", "on_destroy<ScriptComponent>().connect",
           "Lua environments and timers, ScriptEngine::Release" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.hpp", "m_Emitters",
           Release::Exception, "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp",
           "m_Emitters.clear()",
           "GPU state per emitter is released only on scene replacement: the per-emitter material owns a "
           "descriptor pool that VulkanMaterialBackend destroys IMMEDIATELY (not through the allocator's "
           "deletion ring), so dropping one mid-session needs a device idle or a deferred material free. "
           "Open task for WP5 (residency); a streamed-out emitter keeps its buffers until then." },
    } };
} // namespace Desert::Tests::RuntimeHandles
