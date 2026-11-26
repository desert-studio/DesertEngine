#pragma once

// FACTS A FEATURE MODULE REPORTS ABOUT ITS COMPONENTS (UE: a plugin component's own delegates, e.g. the geometry
// collection's OnChaosBreakEvent). The module names the fact by its reflected data type and EVENT and packs the
// event's payload; the Gameplay Framework (ComponentEventSystem, ScriptSystem) hands each published batch on once,
// without naming the module. Published after PhysicsECSSystem's steps (IPhysicsSceneFeature::PublishEvents).

#include <Engine/Reflection/Value.hpp>

#include <Common/Core/Entt.hpp>

#include <cstdint>
#include <vector>

namespace Desert::ECS
{
    struct ComponentEventFact
    {
        entt::entity                   Self  = entt::null;
        const char*                    Data  = ""; ///< The reflected data type, e.g. "DestructibleData"
        const char*                    Event = ""; ///< Its EVENT, e.g. "OnBreak"
        std::vector<Reflection::Value> Args;       ///< The event's payload, in its declared order
    };

    /// The registry's context: this frame's facts; Publication counts the batches.
    struct ComponentEventFactQueue
    {
        std::vector<ComponentEventFact> Facts;
        std::uint64_t                   Publication = 0;
    };
} // namespace Desert::ECS
