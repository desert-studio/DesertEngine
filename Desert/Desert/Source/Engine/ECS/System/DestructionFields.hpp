#pragma once

// FIRING A FIELD ENTITY (UE: AFieldSystemActor + UFieldSystemComponent::ApplyPhysicsField). An entity with one
// of the field components (ECS/DestructionFieldComponents.hpp) is a field PLACED in the scene; firing it builds
// the field at the entity's world pose and hands it to Destruction::DestructionWorld::ApplyField once, at that
// instant. Every caller (the Sequencer's event, a test, gameplay) goes through this one function, and the
// world it acts on is the scene's (PhysicsECSSystem::GetDestructionWorld, null outside Play).

#include <Engine/Destruction/DestructionWorld.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <cstdint>

namespace Desert::ECS
{
    /// Fires every field component the entity has, each once. Success = the number of bodies and pieces the
    /// fields acted on (DestructionWorld::ApplyField, summed); an entity with no field component is an error.
    [[nodiscard]] Common::ResultStr<uint32_t> FireDestructionField( entt::registry& registry, entt::entity entity,
                                                                    Destruction::DestructionWorld& world );
} // namespace Desert::ECS
