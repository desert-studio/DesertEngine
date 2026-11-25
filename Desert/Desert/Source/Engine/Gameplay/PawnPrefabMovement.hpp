#pragma once

// THE PAWN'S MOVEMENT SETTINGS FROM ITS PREFAB (UE: ACharacter's UCharacterMovementComponent + CapsuleComponent,
// read from the DefaultPawnClass on the server and on every client). The prefab's root CharacterController block
// is the one source: the dedicated server (which links no Engine library) and the client's prediction both read
// the .deprefab through this header, so both sides step the same capsule with the same numbers and read the same
// declared action set (CharacterMoveStep.hpp PawnActions). Gameplay, module Core: the server and Engine link it.

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Physics/CharacterControllerComponent.hpp>
#include <Engine/Gameplay/CharacterMoveStep.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Json/ReadWrite.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <vector>

namespace Desert::Gameplay::PrefabMovement
{
    // The keys of a CharacterController block the movement reads. The block states only what differs from
    // ECS::CharacterControllerData's defaults, so an absent key is that default (not a second copy of it here).
    struct CharacterControllerRecord
    {
        std::optional<float>       Radius;
        std::optional<float>       Height;
        std::optional<float>       MaxWalkSpeed;
        std::optional<float>       MaxSprintSpeed;
        std::optional<float>       MaxAcceleration;
        std::optional<float>       BrakingDecelerationWalking;
        std::optional<float>       GroundFriction;
        std::optional<float>       BrakingFrictionFactor;
        std::optional<float>       JumpZVelocity;
        std::optional<float>       AirControl;
        std::optional<float>       GravityScale;
        std::optional<float>       MaxWalkSpeedCrouched;
        std::optional<float>       CrouchedHeight;
        std::optional<float>       MaxSwimSpeed;
        std::optional<std::string> JumpAction;
        std::optional<std::string> SprintAction;
        std::optional<std::string> CrouchAction;
    };
    DESERT_JSON_PARTIAL( CharacterControllerRecord,
                         "a .deprefab record's CharacterController block (ECS::CharacterControllerData)" );

    struct PrefabRecords
    {
        std::vector<Assets::EntityData> Entities;
    };
    DESERT_JSON_PARTIAL( PrefabRecords,
                         "a .deprefab document (header, name, entities); only its records are read" );

    // UE CharacterMovement over the capsule: the cylinder's half height is Height / 2 less the cap's radius.
    [[nodiscard]] inline PawnMoveSettings ToPawnMoveSettings( const ECS::CharacterControllerData& controller )
    {
        PawnMoveSettings settings;
        settings.MaxWalkSpeed               = controller.MaxWalkSpeed;
        settings.MaxSprintSpeed             = controller.MaxSprintSpeed;
        settings.MaxAcceleration            = controller.MaxAcceleration;
        settings.BrakingDecelerationWalking = controller.BrakingDecelerationWalking;
        settings.GroundFriction             = controller.GroundFriction;
        settings.BrakingFrictionFactor      = controller.BrakingFrictionFactor;
        settings.JumpZVelocity              = controller.JumpZVelocity;
        settings.AirControl                 = controller.AirControl;
        settings.GravityScale = controller.GravityScale;
        settings.MaxWalkSpeedCrouched       = controller.MaxWalkSpeedCrouched;
        settings.MaxSwimSpeed               = controller.MaxSwimSpeed;
        settings.CapsuleRadius              = controller.Radius;
        // The cylinder's half heights as the physics world builds the capsule (never below 1 cm; the crouched one
        // never above the standing one) - ECS::CharacterMovement::CylinderHalfHeight.
        settings.CapsuleHalfHeight  = std::max( controller.Height * 0.5f - controller.Radius, 1.0f );
        settings.CrouchedHalfHeight = std::min(
             std::max( controller.CrouchedHeight * 0.5f - controller.Radius, 1.0f ), settings.CapsuleHalfHeight );
        return settings;
    }

    [[nodiscard]] inline PawnMovement ToPawnMovement( const ECS::CharacterControllerData& controller )
    {
        return PawnMovement{ ToPawnMoveSettings( controller ), PawnActionsOf( controller ) };
    }

    // The movement of the pawn prefab at @p prefabFile: its first record with a CharacterController block (the
    // pawn's root). A prefab with none is refused by name - a pawn that has no movement is not a pawn.
    [[nodiscard]] inline Common::ResultStr<PawnMovement>
    ReadPawnPrefabMovement( const std::filesystem::path& prefabFile )
    {
        auto prefab = Common::Json::ReadFile<PrefabRecords>( prefabFile );
        if ( !prefab )
            return Common::MakeError<PawnMovement>(
                 std::format( "pawn prefab '{}': {}", prefabFile.string(), prefab.GetError() ) );
        for ( const Assets::EntityData& record : prefab.GetValue().Entities )
        {
            auto block = record.Components.get( "CharacterController" );
            if ( !block.has_value() )
                continue;
            auto read = Common::Json::Read<CharacterControllerRecord>( Common::Json::Write( block.value() ) );
            if ( !read )
                return Common::MakeError<PawnMovement>( std::format( "pawn prefab '{}': CharacterController: {}",
                                                                     prefabFile.string(), read.GetError() ) );
            const CharacterControllerRecord& stated = read.GetValue();
            ECS::CharacterControllerData     controller;
            const auto                       take = []<typename T>( T& field, const std::optional<T>& value )
            {
                if ( value )
                    field = *value;
            };
            take( controller.Radius, stated.Radius );
            take( controller.Height, stated.Height );
            take( controller.MaxWalkSpeed, stated.MaxWalkSpeed );
            take( controller.MaxSprintSpeed, stated.MaxSprintSpeed );
            take( controller.MaxAcceleration, stated.MaxAcceleration );
            take( controller.BrakingDecelerationWalking, stated.BrakingDecelerationWalking );
            take( controller.GroundFriction, stated.GroundFriction );
            take( controller.BrakingFrictionFactor, stated.BrakingFrictionFactor );
            take( controller.JumpZVelocity, stated.JumpZVelocity );
            take( controller.AirControl, stated.AirControl );
            take( controller.GravityScale, stated.GravityScale );
            take( controller.JumpAction, stated.JumpAction );
            take( controller.SprintAction, stated.SprintAction );
            take( controller.CrouchAction, stated.CrouchAction );
            take( controller.MaxWalkSpeedCrouched, stated.MaxWalkSpeedCrouched );
            take( controller.CrouchedHeight, stated.CrouchedHeight );
            take( controller.MaxSwimSpeed, stated.MaxSwimSpeed );
            return Common::MakeSuccess( ToPawnMovement( controller ) );
        }
        return Common::MakeError<PawnMovement>(
             std::format( "pawn prefab '{}' has no record with a CharacterController", prefabFile.string() ) );
    }
} // namespace Desert::Gameplay::PrefabMovement
