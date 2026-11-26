#include <Engine/Input/LocalPawnInput.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/System/CharacterMovement.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/Gameplay/CharacterMoveStep.hpp>
#include <Engine/Input/LocalPlayerInput.hpp>
#include <Engine/Physics/CharacterControllerComponent.hpp>

#include <glm/geometric.hpp>

#include <cmath>

namespace Desert::Input
{
    Net::InputStep LocalPawnInputStep( Core::Scene& scene, entt::entity entity,
                                       ECS::CharacterControllerComponent& cc, const Gameplay::PawnActions& actions,
                                       const glm::vec3& worldMove )
    {
        Net::InputStep input;
        input.Move = { worldMove.x, worldMove.y, worldMove.z };
        input.Yaw  = glm::length( worldMove ) > 1e-4f ? std::atan2( -worldMove.x, -worldMove.z ) : 0.0f;
        // The possessed pawn reads the local player's devices (UE: only the controlled pawn's input component).
        if ( entity != entt::null && entity == scene.GetPlayerPawn() )
            if ( auto* scripts = scene.FindSystem<ECS::ScriptSystem>() )
            {
                const LocalPlayerInput& player = scripts->PlayerInput();
                for ( std::size_t i = 0; i < actions.Names.size(); ++i )
                    if ( player.ActionHeld( actions.Names[i] ) )
                        input.Actions |= 1u << i;
            }
        if ( cc.JumpRequested )
        {
            input.Actions |= actions.Bit( actions.Jump );
            cc.JumpRequested = false; // one-shot: this step's press
        }
        if ( cc.CrouchRequested )
            input.Actions |= actions.Bit( actions.Crouch );
        if ( cc.Swimming && cc.SwimVertical != 0.0f )
        {
            input.Axes.resize( Gameplay::kSwimVerticalAxis + 1, 0.0f );
            input.Axes[Gameplay::kSwimVerticalAxis] = cc.SwimVertical;
        }
        return input;
    }

    void StepSceneCharacter( Core::Scene& scene, entt::entity entity, ECS::CharacterControllerComponent& cc,
                             Physics::PhysicsWorld& world, const glm::vec3& worldMove, float dt, float& logClock )
    {
        const Gameplay::PawnActions actions = Gameplay::PawnActionsOf( cc.Data );
        const Net::InputStep        input   = LocalPawnInputStep( scene, entity, cc, actions, worldMove );
        ECS::CharacterMovement::Step( cc, world, input, dt );
        if ( entity != scene.GetPlayerPawn() )
            return;
        logClock += dt;
        if ( logClock < 1.0f )
            return;
        logClock = 0.0f;
        // Where the step left the capsule (its centre, world cm), so a run's frames can be matched to the pawn's
        // travel: walk, sprint and crouch walk advance it, standing up in place does not.
        const glm::vec3 at = world.GetCharacterPosition( cc.RuntimeCharacter );
        LOG_INFO( "[Gameplay] local pawn (Gameplay::StepPawnMove): speed {:.0f} cm/s, actions {}{}{}{}, "
                  "capsule centre ({:.0f}, {:.0f}, {:.0f})",
                  cc.CurrentSpeed, actions.Describe( input ), cc.OnGround ? ", grounded" : ", in the air",
                  cc.IsCrouched ? ", crouched" : "", cc.Swimming ? ", swimming" : "", at.x, at.y, at.z );
    }
} // namespace Desert::Input
