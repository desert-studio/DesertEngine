#pragma once

// THE PLACEMENT OF A PAWN BY THE SERVER'S AUTHORITY (NET-TELEPORT-ENCROACH; UE AActor::TeleportTo ->
// UWorld::FindTeleportSpot, AGameModeBase::RestartPlayer -> SpawnActor AdjustIfPossibleButAlwaysSpawn).
//
// Net owns WHEN a pawn is placed (a console teleport, a spawn, a handover) and what the client sees of it (the
// cut, TeleportCount); the GAME owns WHERE a requested point becomes a valid spot (Engine/Gameplay/TeleportSpot.hpp:
// the capsule's encroachment against the collision world, the floor under it) and the SERVER'S WORLD owns whether
// the collision at that point is resident yet (World Partition residency). Both reach the simulation through the
// two interfaces below, so DesertNet stays physics-, world- and game-free (Mesh of One, NET-ARCH §2M).
//
// Contract of the feature: written in full by the architect, edited by no package.

#include <Net/Replication.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Desert::Net
{
    // What the server's world knows about one point, asked by the simulation for a pending placement.
    enum class EPointResidency : std::uint8_t
    {
        NoCell,   // no cell of the world's grid holds the point: nothing will ever be loaded there
        Loading,  // a cell holds it and is not Activated yet (its read is in flight or not started)
        Resident, // every cell whose loose bounds hold the point is Activated: the collision there is in the world
    };

    // The outcome of one residency question, with its reason (errors are values: the check states why, the caller
    // logs it once). Note names the branch taken - "no cell of the world's grid at (x, z) cm", "no cell of the
    // world's grid reaches (x, y, z) cm: ..." (the cells there do not reach the point's height), "cell '...' at
    // (x, z) cm is loading ...", "N cell(s) reaching (x, y, z) cm resident".
    struct PointResidency
    {
        EPointResidency Residency = EPointResidency::NoCell;
        std::string     Note;
    };

    // The server's streamed world as the simulation sees it (ServerWorld implements it over its residency
    // executor's state; UE UWorldPartitionSubsystem::IsStreamingCompleted for one source point).
    class IWorldResidency
    {
    public:
        virtual ~IWorldResidency() = default;
        [[nodiscard]] virtual PointResidency ResidencyAt( const NetVec3& position ) const = 0;
    };

    // The game's answer to "put this pawn at these feet": the state it stands in there, still (velocity zero,
    // Grounded when its feet are on the floor, falling otherwise), with its mode kept - or a refusal naming why.
    // Note is for the server's log in either case: how the spot was found ("capsule in the ground, lifted 278 cm")
    // or why none was ("no floor within 10000 cm below / 5000 cm above (40000, 300, 3000); 9 probe(s)").
    struct PlacedPawn
    {
        std::optional<PawnMoveState> Landed;
        std::string                  Note;

        [[nodiscard]] bool Ok() const
        {
            return Landed.has_value();
        }
    };

    // UE UWorld::FindTeleportSpot as the simulation calls it. @p from: the pawn's state before (its mode - crouched -
    // picks the capsule; its yaw is kept). @p feet: the requested point, the capsule's feet, world cm. A pure
    // function of (from, feet, the collision world now): the simulation calls it only when ResidencyAt says
    // Resident, so the answer is final.
    class IPawnPlacer
    {
    public:
        virtual ~IPawnPlacer() = default;
        [[nodiscard]] virtual PlacedPawn Place( const PawnMoveState& from, const NetVec3& feet ) = 0;
    };

    // A pawn whose destination cell is still loading is held (not stepped, its inputs acked unapplied) this long at
    // most; then the placement is refused with the reason and the pawn goes on where it was. 10 s at the fixed step
    // (UE: the slow-streaming block gives up after its timeout too).
    inline constexpr std::uint32_t kPlacementStreamingTimeoutSteps = static_cast<std::uint32_t>( kNetStepHz * 10.0 );
} // namespace Desert::Net
