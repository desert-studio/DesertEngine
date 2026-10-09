#pragma once

#include <Engine/Physics/PhysicsWorld.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    /// What happened to Self (UE OnComponentHit / OnComponentBeginOverlap / OnComponentEndOverlap). Contact facts
    /// only: a destructible breaking is destruction's own event (DestructionEvents.hpp).
    enum class PhysicsEventKind : uint8_t
    {
        Hit,
        BeginOverlap,
        EndOverlap,
    };

    /**
     * @brief One physics event, named by entity: what a script or a gameplay system answers. A pair event is
     * one event per side that asked for it (UE fires each component's delegate), so Self is always the entity
     * the event is for. Other may no longer be valid: an EndOverlap is also reported for an entity destroyed
     * while overlapping.
     */
    struct PhysicsEvent
    {
        PhysicsEventKind Kind    = PhysicsEventKind::Hit;
        entt::entity     Self    = entt::null;
        entt::entity     Other   = entt::null;
        glm::vec3        Point   = { 0.0f, 0.0f, 0.0f }; ///< World contact point
        glm::vec3        Normal  = { 0.0f, 0.0f, 0.0f }; ///< World, from Self towards Other
        float            Impulse = 0.0f;                 ///< Hit: kg·cm/s
    };

    /// The frame's physics events in the registry's context (registry.ctx): written by PhysicsECSSystem after
    /// its Step, read until its next Update replaces them.
    struct PhysicsEventQueue
    {
        std::vector<PhysicsEvent> Events;
    };

    /**
     * @brief Appends @p events to @p out, named by entity through @p bodies (body → entity). A side whose body
     * is not in @p bodies (a landscape tile, a destructible's piece) names no entity: it is reported as Other
     * as null, and is not given an event of its own.
     */
    inline void NameContactEvents( std::span<const Physics::ContactEvent>                     events,
                                   const std::unordered_map<Physics::BodyHandle, entt::entity>& bodies,
                                   std::vector<PhysicsEvent>&                                  out )
    {
        const auto entityOf = [&]( Physics::BodyHandle body )
        {
            const auto found = bodies.find( body );
            return found == bodies.end() ? entt::entity( entt::null ) : found->second;
        };
        for ( const Physics::ContactEvent& event : events )
        {
            PhysicsEventKind kind = PhysicsEventKind::Hit;
            if ( event.Kind == Physics::ContactEventKind::BeginOverlap )
                kind = PhysicsEventKind::BeginOverlap;
            else if ( event.Kind == Physics::ContactEventKind::EndOverlap )
                kind = PhysicsEventKind::EndOverlap;
            const entt::entity e1 = entityOf( event.Body1 );
            const entt::entity e2 = entityOf( event.Body2 );
            if ( event.Notify1 && e1 != entt::null )
                out.push_back( { kind, e1, e2, event.Point, event.Normal, event.Impulse } );
            if ( event.Notify2 && e2 != entt::null )
                out.push_back( { kind, e2, e1, event.Point, -event.Normal, event.Impulse } );
        }
    }
} // namespace Desert::ECS
