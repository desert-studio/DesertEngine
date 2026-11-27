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
#include <optional>

namespace Desert::Gameplay
{
    // SurfaceAbove's Body is filled from Physics::RayHit::Body here, so the gameplay "no body" value must be the
    // physics world's invalid body: an unfilled answer never poses as a real body.
    static_assert( kNoSurfaceBody == Physics::kInvalidBody,
                   "Gameplay::kNoSurfaceBody must equal Physics::kInvalidBody" );

    // A character @p Handle its world holds now, with what a view of it needs: the profile (its object channel
    // is the floor ray's) and the capsule the world holds for it (PhysicsPawnMoveWorld::ForCharacter proves it
    // once; the view built from it re-checks nothing).
    struct LiveCharacter
    {
        Physics::CharacterHandle    Handle  = Physics::kInvalidCharacter;
        Physics::CollisionProfileId Profile = Physics::kNoProfile;
        PawnCapsule                 Capsule;
    };

    class PhysicsPawnMoveWorld final : public IPawnMoveWorld
    {
    public:
        // The probe capsule in @p world, sized and profiled as the pawn (UE ACharacter's CapsuleComponent, the
        // "Pawn" profile), moved against the world alone; refused, naming the profile, when the world's register
        // has no such profile - its object channel is the one the floor ray traces by.
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
            const Physics::CollisionChannelId floorChannel =
                 world.GetCollisionProfiles().ObjectChannelOf( profile.GetValue() );
            return Common::MakeSuccess( Result(
                 new PhysicsPawnMoveWorld( world, character.GetValue(), CapsuleOf( settings, false ), floorChannel,
                                           Physics::ECharacterContacts::WorldOnly, true ) ) );
        }
        // Standalone Play: the pawn's OWN capsule in the scene's world (UE: the CharacterMovementComponent's
        // UpdatedComponent), in the shape the world holds for it (standing or crouched - the world's record is
        // the one; a view made every step keeps no copy of it). Its profile (CreateCharacter refused none) gives
        // the floor ray's channel; a @p character @p world does not hold (removed, or never made) has none and
        // is refused by its handle.
        [[nodiscard]] static Common::ResultStr<LiveCharacter> ForCharacter( const Physics::PhysicsWorld& world,
                                                                          Physics::CharacterHandle character )
        {
            const Physics::CollisionProfileId profile = world.GetCharacterProfile( character );
            if ( profile == Physics::kNoProfile )
                return Common::MakeError<LiveCharacter>( std::format(
                     "pawn move world: character {} is not a live character of this world", character ) );
            return Common::MakeSuccess( LiveCharacter{ character, profile, CapsuleIn( world, character ) } );
        }
        // The view of a @p live character (ForCharacter), on the caller's stack: it moves where it stands,
        // against the world and the other characters, and is not removed with this view.
        PhysicsPawnMoveWorld( Physics::PhysicsWorld& world, const LiveCharacter& live )
             : PhysicsPawnMoveWorld( world, live.Handle, live.Capsule,
                                     world.GetCollisionProfiles().ObjectChannelOf( live.Profile ),
                                     Physics::ECharacterContacts::WithCharacters, false )
        {
        }
        ~PhysicsPawnMoveWorld() override
        {
            if ( m_Owned )
                m_World.RemoveCharacter( m_Character );
        }
        PhysicsPawnMoveWorld( const PhysicsPawnMoveWorld& )            = delete;
        PhysicsPawnMoveWorld& operator=( const PhysicsPawnMoveWorld& ) = delete;

        // The capsule the character has now; its feet are its centre less HalfHeight + Radius.
        [[nodiscard]] const PawnCapsule& Capsule() const
        {
            return m_Capsule;
        }

        Outcome Move( const PawnMoveState& from, const NetVec3& velocity, const PawnCapsule& capsule,
                      float gravity, float followFloorCm, float dtSeconds ) override
        {
            // The step's capsule (standing or crouched) changes before the move, its feet at from.Position (UE
            // Crouch / UnCrouch: the centre moves by the half-height difference). The centre goes to the new
            // capsule's height above the feet FIRST and the shape changes there: grown at the crouched centre,
            // the standing capsule reaches the half-height difference into the floor and the world refuses it.
            if ( capsule != m_Capsule )
            {
                Place( Center( from.Position, capsule ) );
                if ( m_World.SetCharacterCapsule( m_Character, capsule.Radius, capsule.HalfHeight ) )
                    m_Capsule = capsule;
            }
            const glm::vec3 start = Center( from.Position, m_Capsule );
            Place( start );
            // The probe asks the world only: the server's collision world holds cells, no pawns - the client's
            // scene pawns (its own drawn capsule first: it overlaps the probe at every step) would push the
            // prediction off the server. A scene's own capsule meets the other characters too.
            m_World.UpdateCharacter( m_Character, { velocity.X, velocity.Y, velocity.Z }, gravity, dtSeconds,
                                     m_Contacts, from.Grounded ? followFloorCm : 0.0f );
            const glm::vec3 end = m_World.GetCharacterPosition( m_Character );
            // The velocity after the walls: what the capsule actually travelled (UE: Velocity after the slide
            // is the moved delta over the step).
            const glm::vec3 moved = ( end - start ) / dtSeconds;
            Outcome         outcome;
            outcome.Position = { end.x, end.y - ( m_Capsule.HalfHeight + m_Capsule.Radius ), end.z };
            outcome.Velocity = { moved.x, moved.y, moved.z };
            outcome.Grounded = m_World.IsCharacterOnGround( m_Character );
            outcome.Capsule  = m_Capsule;
            return outcome;
        }

        float Gravity() const override { return m_World.GetGravity(); }

        // A probe 1 cm smaller (radius and half height) than @p capsule, so the floor it stands on does not count
        // as an obstacle; a character's own capsule body is never found by the query. By the pawn's object
        // channel, as the floor ray (UE ComponentEncroachesBlockingGeometry): only bodies that BLOCK the pawn
        // make it not fit - a trigger or volume it overlaps does not.
        bool Fits( const NetVec3& feet, const PawnCapsule& capsule ) const override
        {
            constexpr float kInset = 1.0f;
            const glm::vec3 center = Center( feet, capsule );
            return !m_World.OverlapsCapsule( center, std::max( capsule.Radius - kInset, 1.0f ),
                                             std::max( capsule.HalfHeight - kInset, 0.01f ), m_FloorChannel );
        }

        // The first body straight below @p start that BLOCKS the pawn's object channel (UE FindTeleportSpot
        // traces by the pawn's channel): triggers and volumes the pawn overlaps are passed through, characters'
        // inner bodies are never found - the bodies the capsule stands on. The hit's Y is the floor's.
        std::optional<float> FloorBelow( const NetVec3& start, float downCm ) const override
        {
            return CastFloorRay( start, { 0.0f, -1.0f, 0.0f }, downCm );
        }

        // The same channel straight up from @p start, back faces met and a convex hollow: the first blocking
        // surface within @p upCm and its side - a hit normal pointing down is a front face, a ceiling's
        // underside; one pointing up is the back of a body's top (a heightfield, a mesh floor or a convex the
        // start is under or inside: its exit face's outward normal). A hit with no normal (its body could not be
        // read, CastRay's zero normal) is no top to stand on: an underside. Body: the hit body's handle.
        std::optional<Gameplay::SurfaceAbove> SurfaceAbove( const NetVec3& start, float upCm ) const override
        {
            const std::optional<Physics::RayHit> hit =
                 m_World.CastRay( { start.X, start.Y, start.Z }, { 0.0f, 1.0f, 0.0f }, upCm, m_FloorChannel,
                                  Physics::ERayFaces::Both );
            if ( !hit )
                return std::nullopt;
            return Gameplay::SurfaceAbove{ hit->Point.y, !( hit->Normal.y > 0.0f ), hit->Body };
        }

    private:
        PhysicsPawnMoveWorld( Physics::PhysicsWorld& world, Physics::CharacterHandle character,
                              const PawnCapsule& capsule, Physics::CollisionChannelId floorChannel,
                              Physics::ECharacterContacts contacts, bool owned )
             : m_World( world ), m_Character( character ), m_Capsule( capsule ), m_FloorChannel( floorChannel ),
               m_Contacts( contacts ), m_Owned( owned )
        {
        }
        [[nodiscard]] std::optional<float> CastFloorRay( const NetVec3& start, const glm::vec3& direction,
                                                         float distanceCm ) const
        {
            const std::optional<Physics::RayHit> hit =
                 m_World.CastRay( { start.X, start.Y, start.Z }, direction, distanceCm, m_FloorChannel );
            if ( !hit )
                return std::nullopt;
            return hit->Point.y;
        }
        [[nodiscard]] static PawnCapsule CapsuleIn( const Physics::PhysicsWorld& world,
                                                    Physics::CharacterHandle     character )
        {
            const Physics::CharacterCapsule held = world.GetCharacterCapsule( character );
            return { held.Radius, held.HalfHeight };
        }
        // The centre of @p capsule standing with its feet at @p feet.
        [[nodiscard]] static glm::vec3 Center( const NetVec3& feet, const PawnCapsule& capsule )
        {
            return { feet.X, feet.Y + capsule.HalfHeight + capsule.Radius, feet.Z };
        }
        // The probe is teleported (stopped: it carries no velocity from one pawn to the next); the scene's own
        // capsule is set where it stands.
        void Place( const glm::vec3& center )
        {
            if ( m_Owned )
                m_World.TeleportCharacter( m_Character, center );
            else
                m_World.SetCharacterPosition( m_Character, center );
        }
        Physics::PhysicsWorld&      m_World;
        Physics::CharacterHandle    m_Character;
        PawnCapsule                 m_Capsule;
        Physics::CollisionChannelId m_FloorChannel;
        Physics::ECharacterContacts m_Contacts;
        bool                        m_Owned;
    };
} // namespace Desert::Gameplay
