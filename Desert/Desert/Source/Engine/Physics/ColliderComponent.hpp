#pragma once

#include <Common/Core/Entt.hpp>

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
    // --- Physics (Jolt) ---------------------------------------------------------------------------------
    // A body's collision SHAPE (reflected -> Details UI + serialization). Paired with RigidBodyComponent
    // to be simulated by PhysicsECSSystem.
    struct ColliderData
    {
        REFLECT()

        PROPERTY( DisplayName( "Shape" ), Category( "Collider" ), Summary )
        Physics::ShapeType Shape = Physics::ShapeType::Box;

        PROPERTY( DisplayName( "Half Extents" ), Category( "Collider" ), Length )
        glm::vec3 HalfExtents = { 50.0f, 50.0f, 50.0f }; // Box

        PROPERTY( DisplayName( "Radius" ), Category( "Collider" ), Range( 1.0f, 5000.0f ), Length )
        float Radius = 50.0f; // Sphere / Capsule

        PROPERTY( DisplayName( "Half Height" ), Category( "Collider" ), Range( 1.0f, 5000.0f ), Length )
        float HalfHeight = 50.0f; // Capsule

        PROPERTY( DisplayName( "Capsule Axis" ), Category( "Collider" ) )
        Physics::CapsuleAxis Axis =
             Physics::CapsuleAxis::Y; // Capsule: the body-local axis its cylinder runs along

        PROPERTY( DisplayName( "Center" ), Category( "Collider" ), Length )
        glm::vec3 Center = { 0.0f, 0.0f,
                             0.0f }; // Box / Sphere / Capsule: body-local offset (UE FKShapeElem Center)
    };

    struct ColliderComponent
    {
        COMPONENT( Key( "Collider" ), Block( Data ), Run( ActorsAndUI ) )
        ColliderData Data;
    };
} // namespace Desert::ECS
