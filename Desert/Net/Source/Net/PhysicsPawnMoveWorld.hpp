#pragma once

// THE OWN CHARACTER'S COLLISION WORLD, ONE IMPLEMENTATION FOR BOTH SIDES (PawnMovement.hpp IPawnMoveWorld).
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
// Header-only on purpose: DesertNet is physics-free (the mesh roles, the wire); the two executables that move
// pawns (DesertServer, the engine's ClientNetDriver) link PhysicsCore and include this.

#include <Net/PawnMovement.hpp>
#include <PhysicsCore/CollisionProfiles.hpp>
#include <PhysicsCore/PhysicsWorld.hpp>

#include <Common/Core/ResultStr.hpp>

#include <format>
#include <memory>

namespace Desert::Net
{
    class PhysicsPawnMoveWorld final : public IPawnMoveWorld
    {
    public:
        // The probe capsule in @p world, sized and profiled as the pawn (UE ACharacter's CapsuleComponent, the
        // "Pawn" profile); refused when the world's register has no such profile.
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
            return Common::MakeSuccess( Result( new PhysicsPawnMoveWorld( world, character.GetValue() ) ) );
        }
        ~PhysicsPawnMoveWorld() override
        {
            m_World.RemoveCharacter( m_Character );
        }
        PhysicsPawnMoveWorld( const PhysicsPawnMoveWorld& )            = delete;
        PhysicsPawnMoveWorld& operator=( const PhysicsPawnMoveWorld& ) = delete;

        Outcome Move( const PawnMoveState& from, const NetVec3& velocity, const PawnMoveSettings& settings,
                      float dtSeconds ) override
        {
            const float     feetToCenter = settings.CapsuleHalfHeight + settings.CapsuleRadius;
            const glm::vec3 start{ from.Position.X, from.Position.Y + feetToCenter, from.Position.Z };
            m_World.TeleportCharacter( m_Character, start );
            // World only: the server's collision world holds cells, no pawns - the client's scene pawns (its own
            // drawn capsule first: it overlaps the probe at every step) would push the prediction off the server.
            m_World.UpdateCharacter( m_Character, { velocity.X, velocity.Y, velocity.Z }, settings.Gravity,
                                     dtSeconds, Physics::ECharacterContacts::WorldOnly );
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

    private:
        PhysicsPawnMoveWorld( Physics::PhysicsWorld& world, Physics::CharacterHandle character )
             : m_World( world ), m_Character( character )
        {
        }
        Physics::PhysicsWorld&   m_World;
        Physics::CharacterHandle m_Character;
    };
} // namespace Desert::Net
