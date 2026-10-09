#pragma once

// DESTRUCTION REPORTS ITS OWN BREAKS. Physics reports contact facts (PhysicsEvents.hpp); a piece breaking off a
// destructible is destruction's news, published by destruction's ECS owner (DestructibleLifetime::PublishEvents)
// into its own queue. Port of UE: UGeometryCollectionComponent::DispatchBreakEvent → OnChaosBreakEvent
// (GeometryCollectionComponent.h:1269, .cpp:1501) — the break is the destructible component's event, not the
// physics scene's collision notify.

#include <Engine/Destruction/DestructionWorld.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    /// A piece of Self's destructible broke off (UE FChaosBreakEvent on the geometry collection's component).
    struct DestructionBreakEvent
    {
        entt::entity Self     = entt::null;
        int32_t      Node     = -1;                   ///< The fracture node that broke off
        glm::vec3    Position = { 0.0f, 0.0f, 0.0f }; ///< The piece's world centre of mass, cm
        glm::vec3    Velocity = { 0.0f, 0.0f, 0.0f }; ///< The piece's velocity, cm/s
    };

    /// The frame's destruction events in the registry's context (registry.ctx): written after the physics step
    /// that broke them, read until the next frame's publish replaces them.
    struct DestructionEventQueue
    {
        std::vector<DestructionBreakEvent> Breaks;
    };

    /// Appends the Break events of @p events to @p out, Self the destructible entity of the event's object
    /// (@p objects: DestructibleHandle → entity). Removed events are the simulation's bookkeeping, not news; an
    /// object with no entity names nothing.
    inline void NameBreakEvents( std::span<const Destruction::DestructionEvent>                           events,
                                 const std::unordered_map<Destruction::DestructibleHandle, entt::entity>& objects,
                                 std::vector<DestructionBreakEvent>&                                      out )
    {
        for ( const Destruction::DestructionEvent& event : events )
        {
            if ( event.Kind != Destruction::DestructionEventKind::Break )
                continue;
            const auto found = objects.find( event.Object );
            if ( found == objects.end() )
                continue;
            out.push_back( { found->second, event.Node, event.Position, event.Velocity } );
        }
    }
} // namespace Desert::ECS
