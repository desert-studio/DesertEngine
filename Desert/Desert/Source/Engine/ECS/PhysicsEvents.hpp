#pragma once

#include <Engine/Destruction/DestructionWorld.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    /// What happened to Self (UE OnComponentHit / OnComponentBeginOverlap / OnComponentEndOverlap, Chaos break).
    enum class PhysicsEventKind : uint8_t
    {
        Hit,
        BeginOverlap,
        EndOverlap,
        Break, ///< A piece of Self's destructible broke off (Node names it)
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
        glm::vec3        Point   = { 0.0f, 0.0f, 0.0f }; ///< World contact point / the broken piece's centre
        glm::vec3        Normal  = { 0.0f, 0.0f, 0.0f }; ///< World, from Self towards Other
        float            Impulse = 0.0f;                 ///< Hit: kg·cm/s
        glm::vec3        Velocity = { 0.0f, 0.0f, 0.0f }; ///< Break: the piece's velocity, cm/s
        int32_t          Node     = -1;                   ///< Break: the fracture node
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

    /// Appends the Break events of @p events to @p out, Self the destructible entity of the event's object
    /// (@p objects: DestructibleHandle → entity). Removed events are the simulation's bookkeeping, not news.
    inline void NameBreakEvents( std::span<const Destruction::DestructionEvent>                         events,
                                 const std::unordered_map<Destruction::DestructibleHandle, entt::entity>& objects,
                                 std::vector<PhysicsEvent>&                                              out )
    {
        for ( const Destruction::DestructionEvent& event : events )
        {
            if ( event.Kind != Destruction::DestructionEventKind::Break )
                continue;
            const auto found = objects.find( event.Object );
            if ( found == objects.end() )
                continue;
            PhysicsEvent named;
            named.Kind     = PhysicsEventKind::Break;
            named.Self     = found->second;
            named.Point    = event.Position;
            named.Velocity = event.Velocity;
            named.Node     = event.Node;
            out.push_back( named );
        }
    }
} // namespace Desert::ECS
