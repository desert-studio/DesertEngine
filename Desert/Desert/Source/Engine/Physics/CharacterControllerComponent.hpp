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
    // THE PLAYABLE CHARACTER'S MOVEMENT (GP2a; UE: ACharacter's UCapsuleComponent + UCharacterMovementComponent in
    // MOVE_Walking / MOVE_Falling, crouch included). Drives a Jolt CharacterVirtual capsule (walks slopes/steps,
    // blocked by world geometry); the model itself - acceleration, braking, friction, air control, jump, the
    // crouch shrink and the can-uncrouch overlap - is ECS/System/CharacterMovement.cpp. Every speed is cm/s,
    // every acceleration cm/s^2, every length cm. The INPUT is not here: the pawn's script reads the Enhanced
    // Input actions (IA_Move / IA_Look / IA_Jump / IA_Crouch) and calls self:move / jump / crouch.
    struct CharacterControllerData
    {
        REFLECT()

        PROPERTY( DisplayName( "Radius" ), Category( "Capsule" ), Range( 5.0f, 500.0f ), Length )
        float Radius = 34.0f;

        // Total standing capsule height, caps included (cylinder half-height = (Height - 2*Radius) / 2).
        PROPERTY( DisplayName( "Height" ), Category( "Capsule" ), Range( 20.0f, 1000.0f ), Length )
        float Height = 176.0f;

        PROPERTY( DisplayName( "Max Slope" ), Category( "Capsule" ), Range( 0.0f, 89.0f ), Units( "deg" ) )
        float MaxSlopeDeg = 50.0f;

        // UE MaxWalkSpeed: the ground speed a full stick reaches.
        PROPERTY( DisplayName( "Max Walk Speed" ), Category( "Walking" ), Range( 0.0f, 5000.0f ), Units( "cm/s" ),
                  Summary )
        float MaxWalkSpeed = 600.0f;

        // The ground speed while the pawn's Sprint Action is held (a game's sprint; UE games raise MaxWalkSpeed).
        PROPERTY( DisplayName( "Max Sprint Speed" ), Category( "Walking" ), Range( 0.0f, 5000.0f ),
                  Units( "cm/s" ) )
        float MaxSprintSpeed = 1000.0f;

        // UE MaxAcceleration: how fast input changes the velocity (ground; times Air Control in the air).
        PROPERTY( DisplayName( "Max Acceleration" ), Category( "Walking" ), Range( 0.0f, 20000.0f ),
                  Units( "cm/s2" ) )
        float MaxAcceleration = 2048.0f;

        // UE BrakingDecelerationWalking: the constant deceleration with no input on the ground.
        PROPERTY( DisplayName( "Braking Deceleration" ), Category( "Walking" ), Range( 0.0f, 20000.0f ),
                  Units( "cm/s2" ) )
        float BrakingDecelerationWalking = 2048.0f;

        // UE GroundFriction: turning friction while accelerating, and (times Braking Friction Factor) the
        // velocity-proportional part of braking.
        PROPERTY( DisplayName( "Ground Friction" ), Category( "Walking" ), Range( 0.0f, 100.0f ), Advanced )
        float GroundFriction = 8.0f;

        // UE BrakingFrictionFactor (default 2): ground friction is multiplied by this while braking.
        PROPERTY( DisplayName( "Braking Friction Factor" ), Category( "Walking" ), Range( 0.0f, 10.0f ), Advanced )
        float BrakingFrictionFactor = 2.0f;

        // UE JumpZVelocity: the upward launch speed; the apex is JumpZVelocity^2 / (2 * gravity).
        PROPERTY( DisplayName( "Jump Z Velocity" ), Category( "Jumping / Falling" ), Range( 0.0f, 5000.0f ),
                  Units( "cm/s" ), Summary )
        float JumpZVelocity = 700.0f;

        // UE AirControl: the fraction of Max Acceleration the input has while falling (0 = none, 1 = as on
        // the ground).
        PROPERTY( DisplayName( "Air Control" ), Category( "Jumping / Falling" ), Range( 0.0f, 1.0f ) )
        float AirControl = 0.35f;

        // UE GravityScale: times the scene's gravity (the physics world's), not a gravity of its own.
        PROPERTY( DisplayName( "Gravity Scale" ), Category( "Jumping / Falling" ), Range( 0.0f, 10.0f ) )
        float GravityScale = 1.0f;

        // UE MaxWalkSpeedCrouched.
        PROPERTY( DisplayName( "Max Walk Speed Crouched" ), Category( "Crouching" ), Range( 0.0f, 5000.0f ),
                  Units( "cm/s" ) )
        float MaxWalkSpeedCrouched = 300.0f;

        // Total crouched capsule height, caps included (UE CrouchedHalfHeight x 2). The radius stays.
        PROPERTY( DisplayName( "Crouched Height" ), Category( "Crouching" ), Range( 20.0f, 1000.0f ), Length )
        float CrouchedHeight = 120.0f;

        // UE MaxSwimSpeed: horizontal and vertical speed while the script says the body is swimming.
        PROPERTY( DisplayName( "Max Swim Speed" ), Category( "Swimming" ), Range( 0.0f, 5000.0f ),
                  Units( "cm/s" ) )
        float MaxSwimSpeed = 300.0f;

        // THE PAWN'S DECLARED ACTIONS the movement reads, by Enhanced Input action name (UE: the pawn's
        // SetupPlayerInputComponent bindings; Mover 2.0: the game's input struct). In this order they are the
        // pawn's replicated action set (Engine/Gameplay/CharacterMoveStep.hpp PawnActions): a network input step
        // carries them as bits and the server's step reads them by these names. Empty: the pawn has no such action.
        PROPERTY( DisplayName( "Jump Action" ), Category( "Input" ) )
        std::string JumpAction = "IA_Jump";

        PROPERTY( DisplayName( "Sprint Action" ), Category( "Input" ) )
        std::string SprintAction = "IA_Sprint";

        // The capsule's profile in Config/CollisionProfiles.json (UE: the capsule's Collision Presets, "Pawn").
        PROPERTY( DisplayName( "Collision Profile" ), Category( "Collision" ) )
        std::string CollisionProfile = "Pawn";
    };

    // A playable character. The follow camera is NOT here - a child entity with a SpringArmComponent and its
    // camera child (third person), or a camera child at the head (first person).
    struct CharacterControllerComponent
    {
        COMPONENT( Key( "CharacterController" ), Block( Data ), Run( ActorsAndUI ) )
        CharacterControllerData Data;

        // Transient (Play only): the live Jolt character and the movement state.
        Physics::CharacterHandle RuntimeCharacter = Physics::kInvalidCharacter;
        glm::vec3                Velocity         = { 0.0f, 0.0f, 0.0f }; // cm/s, what the model asked of Jolt
        float                    CurrentSpeed     = 0.0f;  // planar |Velocity|, cm/s (drives locomotion)
        bool                     OnGround         = false; // last physics result (self:isOnGround())
        bool                     IsCrouched       = false; // UE bIsCrouched: the capsule IS the crouched one

        // INTENT, written by the pawn's script each frame and executed by PhysicsECSSystem (UE: AddMovementInput,
        // Jump, Crouch / UnCrouch). MoveInput is camera-relative (x = right, y = forward), length clamped to 1.
        glm::vec2 MoveInput       = { 0.0f, 0.0f };
        bool      JumpRequested   = false; // one-shot, consumed by the next physics step
        bool      CrouchRequested = false; // UE bWantsToCrouch: held; standing up waits for head room

        // Swimming (set by the controller SCRIPT when it detects the body is below the water level): gravity is
        // replaced by the swim intent (+1 up, -1 down) at Max Swim Speed.
        bool  Swimming     = false;
        float SwimVertical = 0.0f;

        // ROOT MOTION (UE ACharacter::RootMotionParams, consumed by the CharacterMovementComponent). Written by
        // AnimationECSSystem when this character's skinned child plays with RootMotionFromEverything — this
        // frame's root movement converted to world space and divided by the animation step (UE
        // CalcAnimRootMotionVelocity) — and consumed by every fixed step of this frame's physics, where it
        // REPLACES the planar velocity the input would have produced (gravity and jumps still act). Cleared by
        // PhysicsECSSystem after the frame's step, so a character whose animation stopped is the input's again.
        // RootMotionRotation is the frame's turn about world up; PhysicsECSSystem — the one writer of a
        // character's transform — applies it with the step's position (UE: the CMC rotates the actor).
        bool      HasRootMotion      = false;
        glm::vec3 RootMotionVelocity = { 0.0f, 0.0f, 0.0f };       // cm/s, world
        glm::quat RootMotionRotation = { 1.0f, 0.0f, 0.0f, 0.0f }; // world yaw delta
    };
} // namespace Desert::ECS
