#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// UE USpringArmComponent::UpdateDesiredArmLocation (GP2a): where the arm's camera goes this frame. Device-free:
// the solve needs a PhysicsWorld only for the probe sweep (Desert/Tests/Engine/CharacterMovement drives it).
namespace Desert::ECS::SpringArm
{
    struct ArmSolve
    {
        glm::vec3 CameraPosition = { 0.0f, 0.0f, 0.0f }; // world space
        float     ArmLength      = 0.0f;                 // origin to camera, after the pull-in
        bool      Blocked        = false;                // the probe met something
    };

    /**
     * @brief The camera's world position for an arm hanging from @p origin with world @p rotation: Target Arm
     * Length along the arm's +Z plus Socket Offset in arm space, from the (lagged, when Enable Camera Lag)
     * origin; with Do Collision Test a sphere of Probe Size swept from the UNLAGGED origin pulls it in to the
     * first hit. Updates @p arm's lagged origin and CurrentArmLength. @p world may be null (no sweep).
     */
    ArmSolve Solve( SpringArmComponent& arm, const glm::vec3& origin, const glm::quat& rotation, float dt,
                    const Physics::PhysicsWorld* world );

    /// Every SpringArmComponent of @p registry: solved from its entity's world transform, and each child with a
    /// CameraComponent placed at the solved position (its local translation in the arm's space).
    void UpdateAll( entt::registry& registry, const Physics::PhysicsWorld* world, float dt );
} // namespace Desert::ECS::SpringArm
