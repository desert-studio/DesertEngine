#pragma once

// THE OWN CHARACTER'S COLLISION WORLD, ONE IMPLEMENTATION FOR BOTH SIDES (CharacterMoveStep.hpp IPawnMoveWorld).
//
// A Jolt CharacterVirtual in a PhysicsWorld (PhysicsCore) moves the capsule: walls slide it, steps and slopes up
// to the walkable angle carry it, a floor under it grounds it (UE SafeMoveUpdatedComponent + FindFloor). The
// server's PhysicsWorld holds the colliders of the cells World Partition streams around its pawns
// (ServerCollision), the client's is the scene's - the same cooked colliders of the same cells - so the same
// input steps take the capsule to the same place on both, and the prediction holds over a fence.
//
// One probe character serves every pawn: each move places it at the pawn's feet first, so the result is a
// function of (state, velocity, world) alone - what the client's replay and the server's step both require.
//
// Gameplay (module Core, PhysicsCore below it): DesertNet stays physics- and game-free (the mesh roles, the wire);
// the server and the engine's ClientNetDriver build their Gameplay::CharacterPawnMover over this world.

#include <Engine/Gameplay/CharacterMoveStep.hpp>
#include <PhysicsCore/CollisionProfiles.hpp>
#include <PhysicsCore/PhysicsWorld.hpp>

#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <format>
#include <memory>

namespace Desert::Gameplay
{
    class PhysicsPawnMoveWorld final : public IPawnMoveWorld
    {
    public:
        // The probe capsule in @p world, sized and profiled as the pawn (UE ACharacter's CapsuleComponent, the
        // "Pawn" profile), moved against the world alone; refused when the world's register has no such profile.
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<PhysicsPawnMoveWorld>>
        Create( Physics::PhysicsWorld& world, const PawnMoveSettings& settings )
        {
            using Result = std::unique_ptr<PhysicsPawnMoveWorld>;
            auto profile = world.GetCollisionProfiles().Resolve( Physics::EngineProfiles::kPawn );
            if ( !profile )
                return Common::MakeError<Result>( std::format( "pawn move world: {}", profile.GetError() ) );
            Physics::CharacterDesc desc;
            desc.Radius     = settings.CapsuleRadius;
            desc.HalfHeight = settings.CapsuleHalfHeight;
            desc.Profile    = profile.GetValue();
            auto character  = world.CreateCharacter( desc );
            if ( !character )
                return Common::MakeError<Result>( std::format( "pawn move world: {}", character.GetError() ) );
            return Common::MakeSuccess(
                 Result( new PhysicsPawnMoveWorld( world, character.GetValue(), CapsuleOf( settings, false ),
                                                   Physics::ECharacterContacts::WorldOnly, true ) ) );
        }
        // Standalone Play: the pawn's OWN capsule in the scene's world (UE: the CharacterMovementComponent's
        // UpdatedComponent), whose shape is @p current now. It moves where it stands, against the world and the
        // other characters, and is not removed with this view.
        PhysicsPawnMoveWorld( Physics::PhysicsWorld& world, Physics::CharacterHandle character,
                              const PawnCapsule& current )
             : PhysicsPawnMoveWorld( world, character, current, Physics::ECharacterContacts::WithCharacters,
                                     false )
        {
        }
        ~PhysicsPawnMoveWorld() override
        {
            if ( m_Owned )
                m_World.RemoveCharacter( m_Character );
        }
        PhysicsPawnMoveWorld( const PhysicsPawnMoveWorld& )            = delete;
        PhysicsPawnMoveWorld& operator=( const PhysicsPawnMoveWorld& ) = delete;

        Outcome Move( const PawnMoveState& from, const NetVec3& velocity, const PawnCapsule& capsule,
                      float gravity, float dtSeconds ) override
        {
            // The step's capsule (standing or crouched): the shape changes before the move, the feet stay.
            if ( ( capsule.Radius != m_Capsule.Radius || capsule.HalfHeight != m_Capsule.HalfHeight ) &&
                 m_World.SetCharacterCapsule( m_Character, capsule.Radius, capsule.HalfHeight ) )
                m_Capsule = capsule;
            const float     feetToCenter = m_Capsule.HalfHeight + m_Capsule.Radius;
            const glm::vec3 start{ from.Position.X, from.Position.Y + feetToCenter, from.Position.Z };
            if ( m_Owned )
                m_World.TeleportCharacter( m_Character, start );
            else
                m_World.SetCharacterPosition( m_Character, start );
            // The probe asks the world only: the server's collision world holds cells, no pawns - the client's
            // scene pawns (its own drawn capsule first: it overlaps the probe at every step) would push the
            // prediction off the server. A scene's own capsule meets the other characters too.
            m_World.UpdateCharacter( m_Character, { velocity.X, velocity.Y, velocity.Z }, gravity, dtSeconds,
                                     m_Contacts );
            const glm::vec3 end = m_World.GetCharacterPosition( m_Character );
            // The velocity after the walls: what the capsule actually travelled (UE: Velocity after the slide
            // is the moved delta over the step).
            const glm::vec3 moved = ( end - start ) / dtSeconds;
            Outcome         outcome;
            outcome.Position = { end.x, end.y - feetToCenter, end.z };
            outcome.Velocity = { moved.x, moved.y, moved.z };
            outcome.Grounded = m_World.IsCharacterOnGround( m_Character );
            return outcome;
        }

        // A probe 1 cm smaller (radius and half height) than @p capsule, so the floor it stands on does not count
        // as an obstacle; a character's own capsule body is never found by the query.
        bool Fits( const NetVec3& feet, const PawnCapsule& capsule ) const override
        {
            constexpr float kInset = 1.0f;
            const glm::vec3 center{ feet.X, feet.Y + capsule.HalfHeight + capsule.Radius, feet.Z };
            return !m_World.OverlapsCapsule( center, std::max( capsule.Radius - kInset, 1.0f ),
                                             std::max( capsule.HalfHeight - kInset, 0.01f ) );
        }

    private:
        PhysicsPawnMoveWorld( Physics::PhysicsWorld& world, Physics::CharacterHandle character,
                              const PawnCapsule& capsule, Physics::ECharacterContacts contacts, bool owned )
             : m_World( world ), m_Character( character ), m_Capsule( capsule ), m_Contacts( contacts ),
               m_Owned( owned )
        {
        }
        Physics::PhysicsWorld&      m_World;
        Physics::CharacterHandle    m_Character;
        PawnCapsule                 m_Capsule;
        Physics::ECharacterContacts m_Contacts;
        bool                        m_Owned;
    };
} // namespace Desert::Gameplay
