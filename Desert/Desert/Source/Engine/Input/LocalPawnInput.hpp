#pragma once

// THE PAWN'S INPUT STEP FROM THE LOCAL PLAYER (NET-GAMEMODE; owner 11-23: the game's logic is not bound to the
// server). UE: the pawn's Enhanced Input bindings feed one CharacterMovementComponent, standalone or as the
// autonomous proxy; Mover 2.0: the game's input producer fills the input struct the movement reads. Here one
// builder makes the Gameplay input step - the move intent in world axes, the pawn's DECLARED actions
// (Gameplay::PawnActions: IA_Jump, IA_Sprint, IA_Crouch from its CharacterController) held in Enhanced Input,
// the swim axis - and both consumers take it: standalone Play steps the scene's character with it
// (StepSceneCharacter -> ECS::CharacterMovement::Step -> Gameplay::StepPawnMove), a network client sends it
// (ClientNetDriver::NextInput) and predicts with the same step. The script's requests (self:jump / crouch / swim,
// UE ACharacter::Jump / Crouch from a Blueprint) press the same declared actions.

#include <Common/Core/Entt.hpp>
#include <glm/vec3.hpp>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::ECS
{
    struct CharacterControllerComponent;
}
namespace Desert::Gameplay
{
    struct PawnActions;
}
namespace Desert::Net
{
    struct InputStep;
}
namespace Desert::Physics
{
    class PhysicsWorld;
}

namespace Desert::Input
{
    /// The input step of @p entity's character this step: @p worldMove (the move intent resolved to world axes)
    /// and its facing; bit i of Actions when @p actions' Names[i] is held in the local player's Enhanced Input
    /// (only for the possessed pawn - another character reads no device) or pressed by its script (the one-shot
    /// Jump request is consumed here; Crouch Requested is held); the script's swim intent on the swim axis.
    [[nodiscard]] Net::InputStep LocalPawnInputStep( Core::Scene& scene, entt::entity entity,
                                                     ECS::CharacterControllerComponent& cc,
                                                     const Gameplay::PawnActions&       actions,
                                                     const glm::vec3&                   worldMove );

    /// One fixed step of a scene character (standalone Play): its input step from its own declared actions,
    /// then ECS::CharacterMovement::Step. The possessed pawn's speed, action set and mode are logged once per
    /// second of @p logClock (the caller's accumulator).
    void StepSceneCharacter( Core::Scene& scene, entt::entity entity, ECS::CharacterControllerComponent& cc,
                             Physics::PhysicsWorld& world, const glm::vec3& worldMove, float dt, float& logClock );
} // namespace Desert::Input
