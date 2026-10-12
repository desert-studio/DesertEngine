#pragma once

// DESTRUCTION REPORTS ITS OWN NEWS. Physics reports contact facts (PhysicsEvents.hpp); a piece breaking off a
// destructible, a contact on its pieces and a piece leaving the simulation are destruction's news, published by
// destruction's ECS owner (DestructibleLifetime::PublishEvents) into its own queue. The Gameplay Framework hands
// them on: ComponentEventSystem to DestructibleData's OnBreak / OnCollision / OnRemoved subscribers, ScriptSystem
// to the entity's scripts' functions of the same names. What a game does with them (a sound, a VFX data channel
// write, a score) is the game's: the engine names no channel. Port of UE: UGeometryCollectionComponent::
// DispatchBreakEvent / DispatchChaosPhysicsCollisionBlueprintEvents / DispatchRemovalEvent → OnChaosBreakEvent /
// OnChaosPhysicsCollision / OnChaosRemovalEvent (GeometryCollectionComponent.h), each gated by the component's
// bNotifyBreaks / bNotifyCollisions / bNotifyRemovals.

#include <Engine/Destruction/DestructionWorld.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    /// A group of Self's pieces broke off as a body of its own (UE FChaosBreakEvent).
    struct DestructionBreakEvent
    {
        entt::entity Self       = entt::null;
        int32_t      Node       = -1;                   ///< The first fracture node of the group
        glm::vec3    Position   = { 0.0f, 0.0f, 0.0f }; ///< The group's world centre of mass, cm
        glm::vec3    Velocity   = { 0.0f, 0.0f, 0.0f }; ///< The group's velocity, cm/s
        float        MassKg     = 0.0f;                 ///< The group's mass
        int32_t      PieceCount = 0;                    ///< Leaves in the group
    };

    /// A contact on one of Self's bodies at or above its Collision Event Min Impulse (UE FChaosCollisionEvent).
    struct DestructionCollisionEvent
    {
        entt::entity Self     = entt::null;
        int32_t      Node     = -1;                   ///< The unit touched
        glm::vec3    Position = { 0.0f, 0.0f, 0.0f }; ///< World contact point, cm
        glm::vec3    Normal   = { 0.0f, 0.0f, 0.0f }; ///< Out of Self's body, towards what it hit
        float        Impulse  = 0.0f;                 ///< The contact's normal impulse, kg*cm/s
        glm::vec3    Velocity = { 0.0f, 0.0f, 0.0f }; ///< The body's velocity, cm/s
        float        MassKg   = 0.0f;                 ///< The body's mass
    };

    /// One of Self's pieces left the simulation: slept long enough, or a kill field (UE FChaosRemovalEvent).
    struct DestructionRemovalEvent
    {
        entt::entity Self       = entt::null;
        int32_t      Node       = -1;                   ///< The member that left
        glm::vec3    Position   = { 0.0f, 0.0f, 0.0f }; ///< Its world centre of mass, cm
        float        MassKg     = 0.0f;                 ///< Its mass
        int32_t      PieceCount = 0;                    ///< Leaves in it
    };

    /// The frame's destruction events in the registry's context (registry.ctx): written after the physics step
    /// that made them, read until the next frame's publish replaces them.
    struct DestructionEventQueue
    {
        std::vector<DestructionBreakEvent>     Breaks;
        std::vector<DestructionCollisionEvent> Collisions;
        std::vector<DestructionRemovalEvent>   Removals;
        /// Bumped by every publish (see PhysicsEventQueue::Publication).
        std::uint64_t Publication = 0;

        void Clear()
        {
            Breaks.clear();
            Collisions.clear();
            Removals.clear();
        }
    };

    /// Who a destructible object's events are for, and which of them it asked for (DestructibleData's
    /// NotifyBreaks / NotifyCollisions / NotifyRemovals).
    struct DestructionEventTarget
    {
        entt::entity Self             = entt::null;
        bool         NotifyBreaks     = false;
        bool         NotifyCollisions = false;
        bool         NotifyRemovals   = false;
    };

    /// Appends each of @p events whose object's target asked for its kind to @p out, Self the destructible
    /// entity of the event's object (@p targets: DestructibleHandle → target). An object with no entity names
    /// nothing.
    inline void NameDestructionEvents(
         std::span<const Destruction::DestructionEvent>                                     events,
         const std::unordered_map<Destruction::DestructibleHandle, DestructionEventTarget>& targets,
         DestructionEventQueue&                                                             out )
    {
        for ( const Destruction::DestructionEvent& event : events )
        {
            const auto found = targets.find( event.Object );
            if ( found == targets.end() )
                continue;
            const DestructionEventTarget& target = found->second;
            switch ( event.Kind )
            {
                case Destruction::DestructionEventKind::Break:
                    if ( target.NotifyBreaks )
                        out.Breaks.push_back( { target.Self, event.Node, event.Position, event.Velocity,
                                                event.MassKg, static_cast<int32_t>( event.PieceCount ) } );
                    break;
                case Destruction::DestructionEventKind::Collision:
                    if ( target.NotifyCollisions )
                        out.Collisions.push_back( { target.Self, event.Node, event.Position, event.Normal,
                                                    event.Impulse, event.Velocity, event.MassKg } );
                    break;
                case Destruction::DestructionEventKind::Removed:
                    if ( target.NotifyRemovals )
                        out.Removals.push_back( { target.Self, event.Node, event.Position, event.MassKg,
                                                  static_cast<int32_t>( event.PieceCount ) } );
                    break;
            }
        }
    }
} // namespace Desert::ECS
