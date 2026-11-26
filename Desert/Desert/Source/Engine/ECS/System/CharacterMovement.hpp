#pragma once

#include <Common/Core/ResultStr.hpp>
#include <PhysicsCore/PhysicsWorld.hpp>

#include <glm/glm.hpp>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/Physics/CharacterControllerComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>

namespace Desert::Net
{
    struct InputStep;
    struct PawnMoveState;
}

// THE SCENE CHARACTER'S MOVEMENT (GP2a, NET-GAMEMODE) - the character's capsule in the scene's PhysicsWorld moved
// by THE one movement step, Gameplay::StepPawnMove (Engine/Gameplay/CharacterMoveStep.hpp: UE's
// UCharacterMovementComponent walking / falling / crouch / swimming / root motion), the same step the server runs
// for a session's pawn and the client's prediction replays. Nothing here integrates: this file adapts the
// component (its numbers, its transient movement state, its capsule) to the step and back. Device-free:
// everything below runs against a PhysicsWorld alone.
namespace Desert::ECS::CharacterMovement
{
    /// The capsule's cylinder half-height (caps excluded) for a total @p height and @p radius, as the physics
    /// world builds it (never below 1 cm).
    [[nodiscard]] float CylinderHalfHeight( float height, float radius );

    /// Creates the character's capsule (standing, in the controller's Collision Profile) centred at @p center
    /// and resets the movement state. Refused by name when the profile is unknown or the world refuses the
    /// capsule.
    Common::ResultStr<Physics::CharacterHandle>
    CreateCharacter( CharacterControllerComponent& cc, Physics::PhysicsWorld& world, const glm::vec3& center );

    /**
     * @brief One fixed step of the character under @p input (Input/LocalPawnInput.cpp builds it: the move
     * intent in world axes, the pawn's declared actions, the swim axis): Gameplay::StepPawnMove over the
     * character's own capsule, with the component's numbers (the scene's gravity times Gravity Scale), its mode
     * (crouched, swimming - the script's switch - and the last step's Jump) and this frame's root motion.
     * Afterwards Velocity is what the world let the capsule do, OnGround / IsCrouched the step's result.
     */
    void Step( CharacterControllerComponent& cc, Physics::PhysicsWorld& world, const Net::InputStep& input,
               float dt );

    /**
     * @brief THE ONE WRITER of a character's movement state (UE: the CharacterMovementComponent the AnimBP reads,
     * on the authority, the autonomous proxy and the simulated proxy alike): Velocity, CurrentSpeed (planar),
     * OnGround, IsCrouched, Swimming and JumpHeld from one move state (Gameplay::kMode* bits in Mode). Fed by
     * Step (standalone Play), by the predictor's current state (the own net pawn) and by the interpolated
     * replicated sample (a proxy, ClientNetDriver). LocomotionSystem publishes what it wrote to the graph.
     * CharacterLibrary::SetSwimming only sets the swim intent the next Step starts from; Step's result,
     * written here, is the state.
     */
    void ApplyMoveState( CharacterControllerComponent& cc, const Net::PawnMoveState& state );
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

    /**
     * @brief THE DRAWN PAWN (UE ACharacter, Gameplay::DrawnPawn): @p pawn's entity at its capsule's CENTRE
     * @p center, and its meshes - the direct children with a StaticMesh or SkinnedMesh (UE ACharacter::Mesh, the
     * capsule's child) - @p meshLift above their authored place, so their feet stay on the floor while the
     * capsule is crouched. Other children (the Spring Arm) ride the capsule. cc.MeshLift is the lift they carry.
     */
    void PlaceDrawnPawn( entt::registry& registry, entt::entity pawn, CharacterControllerComponent& cc,
                         const glm::vec3& center, float meshLift );
    /// Standalone Play: the character drawn at its interpolated centre with the lift of the capsule @p world
    /// holds (the world's shape is the one record of the crouch).
    void PlaceDrawnCharacter( entt::registry& registry, entt::entity pawn, CharacterControllerComponent& cc,
                              const Physics::PhysicsWorld& world );
} // namespace Desert::ECS::CharacterMovement
