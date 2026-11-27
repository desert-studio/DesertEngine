#include <Engine/Gameplay/TeleportSpot.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>

namespace Desert::Gameplay
{
    namespace
    {
        [[nodiscard]] NetVec3 Raised( const NetVec3& point, float upCm )
        {
            return { point.X, point.Y + upCm, point.Z };
        }

        [[nodiscard]] float DistanceCm( const NetVec3& a, const NetVec3& b )
        {
            const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
            return std::sqrt( dx * dx + dy * dy + dz * dz );
        }

        [[nodiscard]] std::string PointText( const NetVec3& point )
        {
            return std::format( "({:.0f}, {:.0f}, {:.0f})", point.X, point.Y, point.Z );
        }

        // Feet standing on a floor at @p floorY under @p point.
        struct Stand
        {
            NetVec3 Feet;
            bool    Grounded = false;
        };

        // The capsule's feet the skin above the floor (Grounded; UE FindFloor's snap). On a slope steeper than the
        // skin clears, the capsule's lowest cap touches the rising side: one radius higher clears every walkable
        // slope, and the pawn falls that last bit onto it (not Grounded). Neither fits: something stands there.
        [[nodiscard]] std::optional<Stand> StandOn( const NetVec3& point, float floorY, const PawnCapsule& capsule,
                                                    const IPawnMoveWorld& world, const TeleportSearch& search )
        {
            const NetVec3 onFloor{ point.X, floorY + search.SkinCm, point.Z };
            if ( world.Fits( onFloor, capsule ) )
                return Stand{ onFloor, true };
            const NetVec3 overFloor{ point.X, floorY + search.SkinCm + capsule.Radius, point.Z };
            if ( world.Fits( overFloor, capsule ) )
                return Stand{ overFloor, false };
            return std::nullopt;
        }

        // One probe's answer. A free point with a floor below keeps its height unless that floor is within the
        // step height (snapped onto it). Every other point is lifted onto the first floor a DOWNWARD ray from
        // LiftCm above finds: a capsule encroaching the world, and a capsule wholly under a heightfield - which
        // overlaps none of its triangles (Fits says free) and has no floor below it, since Jolt finds a
        // heightfield from above only (an upward ray from under it would miss it too).
        struct Probed
        {
            Stand Where;
            bool  Lifted       = false; // the feet stand on the floor found from above the point
            float FloorBelowCm = 0.0f;  // the point's height above the floor found (negative: the floor is above)
        };

        [[nodiscard]] std::optional<Probed> Probe( const NetVec3& point, const PawnCapsule& capsule,
                                                   const PawnMoveSettings& settings, const IPawnMoveWorld& world,
                                                   const TeleportSearch& search )
        {
            if ( world.Fits( point, capsule ) )
            {
                if ( const std::optional<float> floor =
                          world.FloorBelow( Raised( point, search.SkinCm ), search.SkinCm + search.DropCm ) )
                {
                    const float below = point.Y - *floor;
                    if ( below <= settings.MaxStepHeight )
                        if ( const std::optional<Stand> stand = StandOn( point, *floor, capsule, world, search ) )
                            return Probed{ *stand, false, below };
                    return Probed{ Stand{ point, false }, false, below };
                }
            }
            const std::optional<float> floor =
                 world.FloorBelow( Raised( point, search.LiftCm ), search.LiftCm + search.DropCm );
            if ( !floor )
                return std::nullopt;
            const std::optional<Stand> stand = StandOn( point, *floor, capsule, world, search );
            if ( !stand )
                return std::nullopt;
            return Probed{ *stand, true, point.Y - *floor };
        }
    } // namespace

    Common::ResultStr<TeleportSpot> FindTeleportSpot( const PawnMoveState& from, const NetVec3& feet,
                                                      const PawnMovement& movement, IPawnMoveWorld& world,
                                                      const TeleportSearch& search )
    {
        const PawnMoveSettings& settings = movement.Settings;
        const PawnCapsule       capsule  = CapsuleOf( settings, ( from.Mode & kModeCrouched ) != 0 );
        const int               ring     = std::max( search.RingProbes, 0 );

        const auto landed = [&]( const Probed& probed, ETeleportSpot how, std::string note )
        {
            TeleportSpot spot;
            spot.State          = from;
            spot.State.Position = probed.Where.Feet;
            spot.State.Velocity = {};
            spot.State.Grounded = probed.Where.Grounded;
            spot.How            = how;
            spot.MovedCm        = DistanceCm( probed.Where.Feet, feet );
            spot.Note           = std::move( note );
            return Common::MakeSuccess( std::move( spot ) );
        };

        if ( const std::optional<Probed> probed = Probe( feet, capsule, settings, world, search ) )
        {
            const float moved = DistanceCm( probed->Where.Feet, feet );
            if ( probed->Lifted )
                return landed( *probed, ETeleportSpot::Lifted,
                               std::format( "capsule in the ground, lifted {:.0f} cm onto the floor", moved ) );
            if ( probed->Where.Grounded )
                return landed( *probed, ETeleportSpot::Free,
                               std::format( "point free, feet set {:.0f} cm onto the floor below it", moved ) );
            return landed( *probed, ETeleportSpot::Falling,
                           std::format( "point free, floor {:.0f} cm below it: the pawn falls to it",
                                        probed->FloorBelowCm ) );
        }

        // UE FindTeleportSpot's lateral tries: the ring at two radii around the point, at its height.
        const float ringCm = 2.0f * capsule.Radius;
        for ( int i = 0; i < ring; ++i )
        {
            const float angle =
                 2.0f * std::numbers::pi_v<float> * static_cast<float>( i ) / static_cast<float>( ring );
            const NetVec3 point{ feet.X + ringCm * std::cos( angle ), feet.Y,
                                 feet.Z + ringCm * std::sin( angle ) };
            if ( const std::optional<Probed> probed = Probe( point, capsule, settings, world, search ) )
                return landed( *probed, ETeleportSpot::Shifted,
                               std::format( "point blocked with no floor to lift onto, shifted {:.0f} cm to ring "
                                            "probe {} of {}{}",
                                            DistanceCm( probed->Where.Feet, feet ), i + 1, ring,
                                            probed->Where.Grounded ? "" : ", falling to its floor" ) );
        }

        return Common::MakeError<TeleportSpot>(
             std::format( "no spot for the pawn at {}: no free capsule with a floor within {:.0f} cm below or "
                          "{:.0f} cm above it; {} probe(s)",
                          PointText( feet ), search.DropCm, search.LiftCm, 1 + ring ) );
    }

    Net::PlacedPawn PawnTeleporter::Place( const PawnMoveState& from, const NetVec3& feet )
    {
        Common::ResultStr<TeleportSpot> spot = FindTeleportSpot( from, feet, m_Movement, m_World, m_Search );
        Net::PlacedPawn                 placed;
        if ( !spot )
        {
            placed.Note = spot.GetError();
            return placed;
        }
        placed.Landed = spot.GetValue().State;
        placed.Note   = spot.GetValue().Note;
        return placed;
    }
} // namespace Desert::Gameplay
