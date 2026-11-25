#pragma once

// THE PAWN'S MOVEMENT STEP - GAMEPLAY, NOT NET (NET-GAMEMODE; owner 11-23: the game's logic is not bound to the
// server). UE: one UCharacterMovementComponent runs on the server and in the client's prediction; Mover 2.0 lets
// the game declare the input the net layer carries. Here: Net carries an opaque input step (Net/Replication.hpp
// InputStep - the move intent and the pawn's declared action set as bits) and calls the game's Net::IPawnMover;
// THIS module is that mover. It lives in the module Core (render-free), which both the editor's client and
// DesertServer link, so the server steps the pawn and the client predicts it with one compiled step.
//
// StepPawnMove is the one movement step both sides run: the server for every input step it receives (and only
// then - a pawn whose input has not arrived holds, so the state after step N is a function of the inputs up to
// N alone), the client for every input step it sends. Same inputs, same float path, same state: the client's
// prediction matches the server bit for bit unless something only the server knows happened (a teleport, a lost
// input, another authority's push) - then Net::MovePredictor takes the server's state and replays.
//
// The math is UE's CharacterMovementComponent walking / falling (CalcVelocity with ground friction and braking,
// JumpZVelocity, AirControl, gravity) on world centimetres, Y up. Where the velocity takes the capsule is the
// collision world's answer (IPawnMoveWorld; UE SafeMoveUpdatedComponent + FindFloor): both sides ask the same
// cooked collision of the same cells (PhysicsPawnMoveWorld over a PhysicsWorld - the server's built from the
// streamed cells' colliders, the client's the scene's), so a fence stops the pawn on both.

#include <Engine/Physics/CharacterControllerComponent.hpp>
#include <Net/MovePrediction.hpp>
#include <Net/Replication.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Gameplay
{
    using Net::InputStep;
    using Net::NetVec3;
    using Net::PawnMoveState;

    // UE CharacterMovementComponent defaults (cm, cm/s, cm/s^2), the same as CharacterControllerData's; the pawn's
    // prefab overrides them (PawnPrefabMovement.hpp).
    struct PawnMoveSettings
    {
        float MaxWalkSpeed               = 600.0f;
        float MaxSprintSpeed             = 1000.0f; // while the pawn's Sprint action is held
        float MaxAcceleration            = 2048.0f;
        float BrakingDecelerationWalking = 2048.0f;
        float GroundFriction             = 8.0f;
        float BrakingFrictionFactor      = 2.0f;
        float JumpZVelocity              = 700.0f;
        float AirControl                 = 0.35f;
        float Gravity                    = 980.0f;
        // UE ACharacter's CapsuleComponent (34 cm radius, 88 cm half height = 54 cm cylinder + the cap).
        float CapsuleRadius     = 34.0f;
        float CapsuleHalfHeight = 54.0f; // the cylinder's half height, without the caps
    };

    // THE PAWN'S DECLARED ACTION SET (Mover 2.0: the game declares the input struct; UE: the pawn's
    // SetupPlayerInputComponent bindings). The ordered Enhanced Input action names the pawn's movement reads, from
    // its prefab's CharacterController block (Jump Action, Sprint Action): bit i of InputStep::Actions is Names[i]
    // on the client that fills it, on the server that steps it and in the replay, because all three build the set
    // from the same prefab. Net never sees a name; a new action is a new name here, not a new wire bit.
    struct PawnActions
    {
        std::vector<std::string> Names;
        std::string              Jump, Sprint; // the movement's roles: which declared name jumps / sprints

        [[nodiscard]] std::optional<std::size_t> IndexOf( std::string_view name ) const;
        // The action's bit in InputStep::Actions (0 when the pawn does not declare it).
        [[nodiscard]] std::uint32_t Bit( std::string_view name ) const;
        [[nodiscard]] bool          Held( const InputStep& input, std::string_view name ) const;
        // "IA_Sprint#1 IA_Jump#0" - the step's set by name and index, for the logs ("none" when empty).
        [[nodiscard]] std::string Describe( const InputStep& input ) const;
    };
    [[nodiscard]] PawnActions PawnActionsOf( const ECS::CharacterControllerData& controller );

    // Everything the movement of one pawn reads besides its state and input: the numbers and the action set.
    struct PawnMovement
    {
        PawnMoveSettings Settings;
        PawnActions      Actions;
    };

    // The collision world one move asks (UE SafeMoveUpdatedComponent + slide + FindFloor): where a capsule whose
    // feet stand at @p from.Position ends after moving with @p velocity for @p dtSeconds, its velocity after
    // the walls and floors it met, and whether it stands on a walkable floor. Position = the capsule's FEET.
    class IPawnMoveWorld
    {
    public:
        struct Outcome
        {
            NetVec3 Position;
            NetVec3 Velocity;
            bool    Grounded = false;
        };
        virtual ~IPawnMoveWorld()                                                               = default;
        [[nodiscard]] virtual Outcome Move( const PawnMoveState& from, const NetVec3& velocity,
                                            const PawnMoveSettings& settings, float dtSeconds ) = 0;
    };

    // One fixed step of the pawn under one input step.
    [[nodiscard]] PawnMoveState StepPawnMove( const PawnMoveState& from, const InputStep& input,
                                              const PawnMovement& movement, IPawnMoveWorld& world,
                                              float dtSeconds );

    // THE GAME'S MOVER, as the server's simulation and the client's prediction call it (Net::IPawnMover).
    class CharacterPawnMover final : public Net::IPawnMover
    {
    public:
        CharacterPawnMover( PawnMovement movement, IPawnMoveWorld& world )
             : m_Movement( std::move( movement ) ), m_World( world )
        {
        }
        [[nodiscard]] PawnMoveState Step( const PawnMoveState& from, const InputStep& input,
                                          float dtSeconds ) override
        {
            return StepPawnMove( from, input, m_Movement, m_World, dtSeconds );
        }
        [[nodiscard]] std::string DescribeActions( const InputStep& input ) const override
        {
            return m_Movement.Actions.Describe( input );
        }
        [[nodiscard]] const PawnMovement& Movement() const
        {
            return m_Movement;
        }

    private:
        PawnMovement    m_Movement;
        IPawnMoveWorld& m_World;
    };
} // namespace Desert::Gameplay
