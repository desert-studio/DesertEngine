#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <glm/glm.hpp>

// THE PLAYABLE CHARACTER'S MOVEMENT MODEL (GP2a) — UE's UCharacterMovementComponent for MOVE_Walking and
// MOVE_Falling with crouch, on the one controller the engine has (CharacterControllerComponent's Jolt
// CharacterVirtual). The numbers are CharacterControllerData's; the math is UE's CalcVelocity /
// ApplyVelocityBraking (same order, same BRAKE_TO_STOP_VELOCITY, same braking sub-step) so a value tuned in UE
// feels the same here. Device-free: everything below runs against a PhysicsWorld alone, which is how
// Desert/Tests/Engine/CharacterMovement drives it.
namespace Desert::ECS::CharacterMovement
{
    /// UE BRAKE_TO_STOP_VELOCITY: below this planar speed braking stops the character outright (cm/s).
    inline constexpr float kBrakeToStopVelocity = 10.0f;

    /// How much smaller (cm, on radius and on half-height) the stand-up probe is than the standing capsule, so
    /// the floor the crouched capsule stands on does not count as an obstacle above it.
    inline constexpr float kUncrouchProbeInset = 1.0f;

    /// The capsule's cylinder half-height (caps excluded) for a total @p height and @p radius, as the physics
    /// world builds it (never below 1 cm).
    [[nodiscard]] float CylinderHalfHeight( float height, float radius );

    /// Creates the character's capsule (standing, in the controller's Collision Profile) centred at @p center
    /// and resets the movement state. Refused by name when the profile is unknown or the world refuses the capsule.
    Common::ResultStr<Physics::CharacterHandle> CreateCharacter( CharacterControllerComponent& cc, Physics::PhysicsWorld& world,
                          const glm::vec3& center );

    /// UE GetMaxSpeed for the current mode: Max Walk Speed Crouched when crouched, Max Walk Speed otherwise.
    [[nodiscard]] float MaxSpeed( const CharacterControllerComponent& cc );

    /**
     * @brief UE CalcVelocity on the planar velocity @p velocity (cm/s): braking (velocity-proportional
     * @p brakingFriction plus constant @p brakingDeceleration, sub-stepped) when there is no @p acceleration or
     * the speed is over @p maxSpeed; otherwise @p friction turns the velocity toward the acceleration, the
     * acceleration is added and the result clamped to @p maxSpeed times @p analogInput (the stick's length).
     */
    [[nodiscard]] glm::vec3 CalcVelocity( const glm::vec3& velocity, const glm::vec3& acceleration, float maxSpeed,
                                          float analogInput, float friction, float brakingFriction,
                                          float brakingDeceleration, float dt );

    /// UE CanCrouchInCurrentState's other half: would the STANDING capsule fit where the crouched one is
    /// (feet kept when on the ground)? false under a low ceiling.
    [[nodiscard]] bool CanUncrouch( const CharacterControllerComponent& cc, const Physics::PhysicsWorld& world );

    /**
     * @brief One frame of the character: crouch / stand up as CrouchRequested asks (standing up only with head
     * room), then walking (acceleration, braking, friction) on the ground or falling (Air Control, gravity
     * times Gravity Scale) in the air, Jump Z Velocity on a requested jump from the ground while standing, and
     * the capsule moved through Jolt. @p worldInput is the move intent already resolved to world axes (its Y is
     * ignored, its length clamped to 1). Afterwards Velocity is what Jolt actually let the capsule do.
     */
    void Step( CharacterControllerComponent& cc, Physics::PhysicsWorld& world, const glm::vec3& worldInput,
               float dt );
    /// The AnimGraph parameters the movement state is published under (UE: the AnimBP's Speed / IsFalling /
    /// IsCrouching read from the CharacterMovementComponent each update). A graph that wants locomotion
    /// declares them: Speed a Float in cm/s (planar), IsFalling and IsCrouched Bools.
    inline constexpr const char* kAnimParamSpeed      = "Speed";
    inline constexpr const char* kAnimParamIsFalling  = "IsFalling";
    inline constexpr const char* kAnimParamIsCrouched = "IsCrouched";

    /**
     * @brief Publishes this frame's movement state into @p anim's AnimGraph: Speed = CurrentSpeed (cm/s),
     * IsFalling = !OnGround (swimming is not falling), IsCrouched. Only the parameters the graph DECLARES are
     * written (a graph without locomotion is not told about it, and the drain would refuse an undeclared name);
     * nothing is written before the graph is loaded. The write goes through AnimationComponent's pending
     * parameter queue, the one path scripts use too, and replaces a value still queued under the same name, so
     * frames before the evaluator exists do not pile up. This is the ONE path from movement to animation: the
     * graph picks and blends the clips.
     */
    void PublishAnimGraphParameters( const CharacterControllerComponent& cc, AnimationComponent& anim );
} // namespace Desert::ECS::CharacterMovement
