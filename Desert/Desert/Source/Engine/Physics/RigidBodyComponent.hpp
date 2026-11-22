#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <PhysicsCore/PhysicsTypes.hpp>

namespace Desert::ECS
{
    // Body simulation params (reflected -> Details UI + serialization).
    struct RigidBodyData
    {
        REFLECT()

        PROPERTY( DisplayName( "Type" ), Category( "Rigid Body" ), Summary )
        Physics::BodyType Type = Physics::BodyType::Dynamic;

        PROPERTY( DisplayName( "Mass" ), Category( "Rigid Body" ), Range( 0.0f, 1000.0f ), Units( "kg" ), Summary )
        float Mass = 1.0f;

        PROPERTY( DisplayName( "Friction" ), Category( "Rigid Body" ), Range( 0.0f, 2.0f ), Advanced )
        float Friction = 0.5f;

        PROPERTY( DisplayName( "Restitution" ), Category( "Rigid Body" ), Range( 0.0f, 1.0f ), Advanced )
        float Restitution = 0.1f;

        // UE's Collision Presets: a profile NAME of the project's Config/CollisionProfiles.json, resolved when
        // the body is created; a name the register lacks refuses the body by name (no fallback profile).
        PROPERTY( DisplayName( "Collision Profile" ), Category( "Collision" ) )
        std::string CollisionProfile = "PhysicsActor";

        // THE BODY'S EVENTS — UE UPrimitiveComponent::OnComponentHit / OnComponentBeginOverlap /
        // OnComponentEndOverlap (PrimitiveComponent.h). Physics reports the facts (PhysicsEventQueue); the
        // Gameplay Framework delivers them here (ComponentEventSystem) to whoever subscribed to THIS entity
        // (ComponentEvents). The profile decides whether a body reports at all (Generates Hit/Overlap Events).
        EVENT( Category( "Collision" ),
               Tooltip( "This body struck another and the contact began (UE OnComponentHit). Normal points from "
                        "this body towards the other; impulse in kg*cm/s." ) )
        using OnHit = void( entt::entity other, glm::vec3 point, glm::vec3 normal, float impulse );

        EVENT( Category( "Collision" ),
               Tooltip( "Another body began to overlap this one (UE OnComponentBeginOverlap)." ) )
        using OnBeginOverlap = void( entt::entity other );

        EVENT( Category( "Collision" ),
               Tooltip( "Another body stopped overlapping this one, also when it was destroyed while overlapping "
                        "(UE OnComponentEndOverlap)." ) )
        using OnEndOverlap = void( entt::entity other );
    };

    // Marks an entity as a physics body. Static = immovable, Dynamic = simulated, Kinematic = code-driven.
    struct RigidBodyComponent
    {
        COMPONENT( Key( "RigidBody" ), Block( Data ), Run( ActorsAndUI ) )
        RigidBodyData Data;

        // Transient: the live Jolt body (created on Play, cleared on Stop). Not reflected/serialized.
        Physics::BodyHandle RuntimeBody = Physics::kInvalidBody;
    };
} // namespace Desert::ECS
