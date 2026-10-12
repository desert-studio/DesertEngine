#pragma once

// KINEMATIC BODIES FOLLOW THEIR ENTITY. A Kinematic RigidBody is moved by its entity's world transform (UE: a
// kinematic body follows its component), handed to the world as a kinematic target so the body travels over
// the step and contacts and overlaps see it move rather than teleport.
//
// Kept out of PhysicsECSSystem.hpp so a suite can drive it with a bare registry and a real PhysicsWorld — that
// header includes Scene, and Scene includes the renderer.

#include <Engine/Physics/PhysicsWorld.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Desert::ECS
{
    /// An entity's pose in the world: its transform composed with every parent's, scale split off.
    struct EntityWorldPose
    {
        glm::vec3 Position = glm::vec3( 0.0f );
        glm::quat Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        glm::vec3 Scale    = glm::vec3( 1.0f );
    };

    [[nodiscard]] EntityWorldPose ComputeEntityWorldPose( const entt::registry& registry, entt::entity entity );

    /// Hands every Kinematic RigidBody's world pose to @p world as its kinematic target. Call before Step.
    void DriveKinematicBodies( entt::registry& registry, Physics::PhysicsWorld& world );
} // namespace Desert::ECS
