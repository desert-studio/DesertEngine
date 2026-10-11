#pragma once

// THE GAMEPLAY FRAMEWORK DELIVERS THE FACTS. Physics reports contacts (PhysicsEventQueue) and destruction reports
// breaks (DestructionEventQueue) as facts named by entity; this system turns each fact into the EVENT of the
// component it happened to — RigidBodyData::OnHit / OnBeginOverlap / OnEndOverlap, DestructibleData::OnBreak —
// and calls whoever subscribed to that entity (ComponentEvents). UE: UPrimitiveComponent::DispatchBlockingHit ->
// OnComponentHit.Broadcast, and the geometry collection's DispatchBreakEvent -> OnChaosBreakEvent.Broadcast; the
// physics scene never calls gameplay itself.
//
// Runs after PhysicsECSSystem (which publishes both queues). Nothing subscribed = nothing done: no payload is
// packed for an entity nobody listens to. Each published batch is delivered once (the queues' Publication).

#include <Engine/ECS/System/System.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::ECS
{
    /// Which batches of the two fact queues have been delivered.
    struct ComponentEventCursor
    {
        std::uint64_t Physics     = 0;
        std::uint64_t Destruction = 0;
    };

    /// Delivers the batches published since @p cursor to the subscribers of @p registry's ComponentEvents and
    /// advances @p cursor. Returns the number of listener calls made.
    std::size_t DeliverComponentEvents( entt::registry& registry, ComponentEventCursor& cursor );

    class ComponentEventSystem final : public System
    {
    public:
        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ) override
        {
            DeliverComponentEvents( registry, m_Cursor );
        }

    private:
        ComponentEventCursor m_Cursor;
    };
} // namespace Desert::ECS
