#pragma once

// THE TELEPORT SPOT - GAMEPLAY, NOT NET (NET-TELEPORT-ENCROACH; UE UWorld::FindTeleportSpot +
// UCharacterMovementComponent::FindFloor on the first move after TeleportTo).
//
// A requested point is not a place a capsule can stand: it may be inside the landscape (the heightfield is above
// it), inside a wall, in the air above a floor, or over nothing. This module turns the point into the state the
// pawn stands in - or refuses with the reason - against the same collision world the pawn moves through
// (IPawnMoveWorld, the server's streamed cells), so a teleport onto the terrain lands ON the terrain and a teleport
// over the void is refused instead of a fall for ever. Lives in Gameplay (module Core, render-free) as
// CharacterMoveStep does: the server links it; a standalone Play's relocation may call the same function.
//
// Contract of the feature: written in full by the architect, edited by no package.

#include <Engine/Gameplay/CharacterMoveStep.hpp>
#include <Net/PawnTeleport.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace Desert::Gameplay
{
    // The search around a requested point (cm). UE FindTeleportSpot tries the penetration adjustment, then
    // offsets in the plane; here the lift answers "in the ground" and the ring answers "in a wall".
    struct TeleportSearch
    {
        float LiftCm    = 5000.0f;  // an encroached capsule is lifted onto the first floor this far above at most
        float DropCm    = 10000.0f; // a free capsule needs a floor this far below at most; none = refused
        float SkinCm    = 2.0f;     // the landed feet stand this far above the floor hit (the capsule's skin)
        int   RingProbes = 8;       // lateral attempts at 2 x radius around the point when it is in a wall
    };

    // What FindTeleportSpot decided, for the server's log: one of these words and the distances.
    enum class ETeleportSpot : std::uint8_t
    {
        Free,     // the point was free; its feet were snapped onto the floor within MaxStepHeight (Grounded)
        Falling,  // the point was free with a floor farther below than MaxStepHeight: the pawn falls to it
        Lifted,   // the capsule was in the ground / an obstacle: raised onto the first floor above the point
        Shifted,  // the point and the lift failed: a ring probe beside it stood
    };

    struct TeleportSpot
    {
        PawnMoveState State;    // Position = the landed feet; Velocity zero; Grounded set; Yaw and Mode = from's
        ETeleportSpot How = ETeleportSpot::Free;
        float         MovedCm = 0.0f; // |landed feet - requested feet|
        std::string   Note;           // "capsule in the ground, lifted 278 cm" - the server's log words
    };

    // The spot for a capsule of @p movement's settings (crouched when from.Mode says so) whose feet are asked to
    // stand at @p feet, against @p world now. An error names why none was found and what was tried.
    [[nodiscard]] Common::ResultStr<TeleportSpot> FindTeleportSpot( const PawnMoveState& from, const NetVec3& feet,
                                                                    const PawnMovement&    movement,
                                                                    IPawnMoveWorld& world,
                                                                    const TeleportSearch& search );

    // THE GAME'S PLACER, as the server's simulation calls it (Net::IPawnPlacer): FindTeleportSpot over the pawn
    // prefab's movement and the server's collision world - the same two CharacterPawnMover steps with.
    class PawnTeleporter final : public Net::IPawnPlacer
    {
    public:
        PawnTeleporter( PawnMovement movement, IPawnMoveWorld& world, TeleportSearch search )
             : m_Movement( std::move( movement ) ), m_World( world ), m_Search( search )
        {
        }
        [[nodiscard]] Net::PlacedPawn Place( const PawnMoveState& from, const NetVec3& feet ) override;

    private:
        PawnMovement    m_Movement;
        IPawnMoveWorld& m_World;
        TeleportSearch  m_Search;
    };
} // namespace Desert::Gameplay
