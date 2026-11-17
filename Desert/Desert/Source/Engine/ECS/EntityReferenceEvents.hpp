#pragma once

// A REFERENCE ACROSS A LOAD BOUNDARY GAINED OR LOST ITS TARGET (WP-ENTITY-REFS).
//
// World Partition streams entities in and out by cell: Play and a packaged game through WorldStreamer (the
// residency executor's ResidencyTick::Rebound / Unbound), the editor through its loaded regions (EditorRegions).
// Most references re-resolve by themselves - every reader of a register field (Rules::kEntityReferences) looks its
// target up by id when it runs. A running script does not: its `Properties.<name>` holds the entity object it was
// handed, so an entity that arrives after the script started would stay nil for good, and one that left would stay
// a dead handle. This queue carries each such change to ScriptSystem, which writes the property again
// (ScriptEngine::RebindEntityProperties) - UE's soft reference resolving while its actor is loaded.
//
// By id, not by entt handle: the holder is found again when the queue is drained, so a holder destroyed in between
// is simply not there. Appended by the publishers, drained whole by ScriptSystem each update - it is the one
// consumer, and a world that runs no script (edit time) drains it too, so nothing accumulates.

#include <Common/Core/UUID.hpp>

#include <entt/entt.hpp>

#include <span>
#include <vector>

namespace Desert::ECS
{
    struct EntityReferenceBinding
    {
        Common::UUID Holder; // the entity whose reference names Target
        Common::UUID Target; // the entity that arrived (bound) or left (unbound) - the property reads it again
    };

    struct EntityReferenceBindings
    {
        std::vector<EntityReferenceBinding> Pending;
    };

    // Appends @p bindings to @p registry's queue (made on first use).
    inline void PublishEntityReferenceBindings( entt::registry&                         registry,
                                                std::span<const EntityReferenceBinding> bindings )
    {
        if ( bindings.empty() )
            return;
        EntityReferenceBindings* queue = registry.try_ctx<EntityReferenceBindings>();
        if ( queue == nullptr )
            queue = &registry.set<EntityReferenceBindings>();
        queue->Pending.insert( queue->Pending.end(), bindings.begin(), bindings.end() );
    }
} // namespace Desert::ECS
