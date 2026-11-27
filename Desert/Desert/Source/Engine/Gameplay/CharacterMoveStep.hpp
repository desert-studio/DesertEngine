#pragma once

// THE PAWN'S MOVEMENT STEP - GAMEPLAY, NOT NET (NET-GAMEMODE; owner 11-23: the game's logic is not bound to the
// server). UE: one UCharacterMovementComponent runs on the server and in the client's prediction; Mover 2.0 lets
// the game declare the input the net layer carries. Here: Net carries an opaque input step (Net/Replication.hpp
// InputStep - the move intent and the pawn's declared action set as bits) and calls the game's Net::IPawnMover;
// THIS module is that mover. It lives in the module Core (render-free), which both the editor's client and
// DesertServer link, so the server steps the pawn and the client predicts it with one compiled step.
//
// StepPawnMove is the one movement step every side runs - standalone Play (ECS::CharacterMovement::Step over the
// pawn's own capsule), the server and the client's prediction (UE: one
// UCharacterMovementComponent::PerformMovement for standalone, authority and autonomous proxy). Walking, falling,
// crouch, swimming and root motion all live in it: StepPawnMove is the one movement step both sides of a session
// run: the server for every input step it receives (and only then - a pawn whose input has not arrived holds, so
// the state after step N is a function of the inputs up to N alone), the client for every input step it sends.
// Same inputs, same float path, same state: the client's prediction matches the server bit for bit unless
// something only the server knows happened (a teleport, a lost input, another authority's push) - then
// Net::MovePredictor takes the server's state and replays.
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
        float GravityScale               = 1.0f; // UE GravityScale: times the world's (IPawnMoveWorld::Gravity)
        float MaxStepHeight              = 45.0f;  // UE MaxStepHeight: a walk follows its floor this far down
        float MaxWalkSpeedCrouched       = 300.0f; // UE MaxWalkSpeedCrouched
        float MaxSwimSpeed               = 300.0f; // UE MaxSwimSpeed, horizontal and vertical
        // UE ACharacter's CapsuleComponent (34 cm radius, 88 cm half height = 54 cm cylinder + the cap).
        float CapsuleRadius      = 34.0f;
        float CapsuleHalfHeight  = 54.0f; // the cylinder's half height, without the caps
        float CrouchedHalfHeight = 26.0f; // UE CrouchedHalfHeight, as the cylinder's (120 cm tall)
    };

    // The game's movement mode bits in PawnMoveState::Mode (Net carries them opaque, beside Grounded). Swimming is
    // the game's mode switch (UE SetMovementMode(MOVE_Swimming): the pawn's script, from the water level); the
    // step keeps it. JumpHeld is the last step's Jump action (UE bPressedJump): one jump a press, on every side.
    inline constexpr std::uint8_t kModeCrouched = 1u << 0;
    inline constexpr std::uint8_t kModeSwimming = 1u << 1;
    inline constexpr std::uint8_t kModeJumpHeld = 1u << 2;
    // The fastest a falling pawn goes down (cm/s; UE APhysicsVolume::TerminalVelocity's default 4000, the cap
    // UCharacterMovementComponent::GetMaxSpeed applies while falling). A constant of the step on every side, so
    // the prediction stays bit-exact; a pawn dropped from any height reaches the ground at this speed at most.
    inline constexpr float kTerminalVelocityCmPerS = 4000.0f;
    // The input step's axis the swim's vertical intent rides (+1 up, -1 down; absent = 0).
    inline constexpr std::size_t kSwimVerticalAxis = 0;

    // THE PAWN'S DECLARED ACTION SET (Mover 2.0: the game declares the input struct; UE: the pawn's
    // SetupPlayerInputComponent bindings). The ordered Enhanced Input action names the pawn's movement reads, from
    // its prefab's CharacterController block (Jump Action, Sprint Action): bit i of InputStep::Actions is Names[i]
    // on the client that fills it, on the server that steps it and in the replay, because all three build the set
    // from the same prefab. Net never sees a name; a new action is a new name here, not a new wire bit.
    struct PawnActions
    {
        std::vector<std::string> Names;
        std::string Jump, Sprint, Crouch; // the movement's roles: which declared name jumps / sprints / crouches

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

    // The pawn's capsule: radius and the cylinder's half height (caps excluded).
    struct PawnCapsule
    {
        float Radius     = 34.0f;
        float HalfHeight = 54.0f;

        friend bool operator==( const PawnCapsule&, const PawnCapsule& ) = default;
    };
    [[nodiscard]] PawnCapsule CapsuleOf( const PawnMoveSettings& settings, bool crouched );

    // THE DRAWN PAWN (UE ACharacter): the pawn's entity is its capsule's CENTRE on every side - standalone Play,
    // the owner and the proxies - and a crouch lifts the drawn meshes by the half-height difference (UE
    // OnStartCrouch: the Mesh's BaseTranslationOffset + HalfHeightAdjust), so their feet stay on the floor while
    // the capsule shrinks; the camera rides the capsule. From the capsule the pawn moved with.
    struct DrawnPawn
    {
        float CenterAboveFeet = 0.0f; // centre = feet + this, up: the half height with the cap
        float MeshLift        = 0.0f; // the meshes above their authored place: standing - held half height
    };
    [[nodiscard]] DrawnPawn DrawnPawnOf( const PawnMoveSettings& settings, const PawnCapsule& capsule );
    // The same from a move mode (PawnMoveState::Mode): its crouch bit picks the capsule.
    [[nodiscard]] DrawnPawn DrawnPawnOf( const PawnMoveSettings& settings, std::uint8_t mode );

    // The first surface an upward ray met (IPawnMoveWorld::SurfaceAbove): its Y, and whether the ray met its front
    // face - the Underside of a ceiling or a roof - rather than its back face, the top of a body the start is
    // under or inside.
    struct SurfaceAbove
    {
        float Y         = 0.0f;
        bool  Underside = false;
    };

    // The collision world one move asks (UE SafeMoveUpdatedComponent + slide + FindFloor): where @p capsule whose
    // feet stand at @p from.Position ends after moving with @p velocity for @p dtSeconds, its velocity after
    // the walls and floors it met, and whether it stands on a walkable floor (@p gravity presses the floor's
    // bodies). A capsule that stood on a floor (@p from.Grounded) and does not rise follows a walkable floor up to
    // @p followFloorCm below (UE PhysWalking's FindFloor down to MaxStepHeight; 0 = none, the swim). Position =
    // the capsule's FEET. A @p capsule other than the one the world holds is a crouch or a stand-up: the world
    // changes the shape with its feet at @p from.Position (UE Crouch / UnCrouch, bCrouchMaintainsBaseLocation:
    // the centre moves by the half-height difference) - or keeps the old one when the new would penetrate it,
    // and Outcome::Capsule says which capsule moved. Fits: would @p capsule stand at @p feet without overlapping
    // the world (UE: the stand-up test of UnCrouch)? FloorBelow: the Y of the first surface the pawn stands on
    // straight below @p start within @p downCm, nullopt for none (UE FindFloor's line trace; the teleport spot's
    // floor, FindTeleportSpot), by the pawn's channel: a trigger or volume it overlaps is no floor. A downward
    // ray: a heightfield is found from above only. SurfaceAbove: the same channel straight up, the first
    // blocking surface within @p upCm and the side the ray met (UE FHitResult::bStartPenetrating): its BACK face
    // - the top of a heightfield, a mesh floor or a convex the start is under or inside - or its front face, the
    // underside of a ceiling or a roof (FindTeleportSpot tells "under the ground" from "under a roof" by it).
    class IPawnMoveWorld
    {
    public:
        struct Outcome
        {
            NetVec3     Position;
            NetVec3     Velocity;
            bool        Grounded = false;
            PawnCapsule Capsule; // the capsule that moved: the step's, or the one the world kept
        };
        virtual ~IPawnMoveWorld()                                                                        = default;
        [[nodiscard]] virtual Outcome Move( const PawnMoveState& from, const NetVec3& velocity,
                                            const PawnCapsule& capsule, float gravity, float followFloorCm,
                                            float dtSeconds )                                            = 0;
        [[nodiscard]] virtual bool    Fits( const NetVec3& feet, const PawnCapsule& capsule ) const      = 0;
        [[nodiscard]] virtual std::optional<float> FloorBelow( const NetVec3& start, float downCm ) const = 0;
        [[nodiscard]] virtual std::optional<Gameplay::SurfaceAbove> SurfaceAbove( const NetVec3& start,
                                                                                    float upCm ) const = 0;
        // The world's gravity (cm/s^2, down; UE AWorldSettings::GetGravityZ): the level's Scene Settings, the same
        // value on the server (its cooked world), the prediction and standalone Play (the scene's physics world).
        [[nodiscard]] virtual float Gravity() const = 0;
    };

    // One fixed step of the pawn under one input step: crouch / stand up (UE UpdateCharacterStateBeforeMovement),
    // then swimming, walking or falling, the capsule moved through @p world. @p rootMotionVelocity (cm/s, world;
    // UE HasAnimRootMotion) replaces the planar velocity the input would give; the session's sides pass none
    // (root motion is not replicated).
    [[nodiscard]] PawnMoveState StepPawnMove( const PawnMoveState& from, const InputStep& input,
                                              const PawnMovement& movement, IPawnMoveWorld& world, float dtSeconds,
                                              const NetVec3* rootMotionVelocity = nullptr );

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
