#include <Engine/Gameplay/TeleportSpot.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <optional>
#include <string>

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

        // One probe's answer, the cases in order (UE FindTeleportSpot / ComponentEncroachesBlockingGeometry: the
        // traces go by the pawn's channel, an unresolved spot is refused - never pushed onto whatever is above).
        //  Under: a DOWNWARD ray from LiftCm above finds a floor above the point and an UPWARD ray from the point
        //    to it misses - the point is under a one-sided surface (a heightfield or a mesh floor seen from below,
        //    whose faces a ray from under them passes): the feet are lifted onto it. A roof or a ceiling above is
        //    found by the upward ray, so the pawn under it is not put on it.
        //  Otherwise the floor straight below within DropCm: a free point keeps its height unless that floor is
        //    within the step height (snapped onto it); an encroached point (feet in the floor) is set onto a floor
        //    within the step height, else it is blocked. No floor below a free point: refused.
        enum class EProbed : std::uint8_t
        {
            Under,     // lifted onto the one-sided surface above the point
            Snapped,   // a free point's feet set onto the floor within the step height below it
            Falling,   // a free point kept, its floor farther below
            Encroached // a blocked point's feet set onto the floor within the step height of it
        };
        struct Probed
        {
            Stand   Where;
            EProbed Case         = EProbed::Snapped;
            float   FloorBelowCm = 0.0f; // the point's height above the floor used (negative: the floor is above)
        };
        // A probe either stands or says why not.
        struct ProbeResult
        {
            std::optional<Probed> Stood;
            std::string           Why;
        };

        [[nodiscard]] ProbeResult Probe( const NetVec3& point, const PawnCapsule& capsule,
                                         const PawnMoveSettings& settings, const IPawnMoveWorld& world,
                                         const TeleportSearch& search )
        {
            if ( const std::optional<float> above =
                      world.FloorBelow( Raised( point, search.LiftCm ), search.LiftCm );
                 above && *above > point.Y )
            {
                if ( !world.CeilingAbove( point, *above - point.Y ) )
                {
                    if ( const std::optional<Stand> stand = StandOn( point, *above, capsule, world, search ) )
                        return { Probed{ *stand, EProbed::Under, point.Y - *above }, {} };
                    return { std::nullopt, std::format( "under the ground {:.0f} cm below its surface, and the "
                                                        "capsule does not fit on it",
                                                        *above - point.Y ) };
                }
            }
            const bool                 fits = world.Fits( point, capsule );
            const std::optional<float> floor =
                 world.FloorBelow( Raised( point, search.SkinCm ), search.SkinCm + search.DropCm );
            if ( !floor )
                return { std::nullopt,
                         fits ? std::format( "free, with no floor within {:.0f} cm below it", search.DropCm )
                              : std::format( "blocked, with no floor within {:.0f} cm below it", search.DropCm ) };
            const float below = point.Y - *floor;
            if ( below <= settings.MaxStepHeight )
                if ( const std::optional<Stand> stand = StandOn( point, *floor, capsule, world, search ) )
                    return { Probed{ *stand, fits ? EProbed::Snapped : EProbed::Encroached, below }, {} };
            if ( fits )
                return { Probed{ Stand{ point, false }, EProbed::Falling, below }, {} };
            return { std::nullopt, std::format( "blocked, its floor {:.0f} cm below it", below ) };
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

        const ProbeResult centre = Probe( feet, capsule, settings, world, search );
        if ( const std::optional<Probed>& probed = centre.Stood )
        {
            const float moved = DistanceCm( probed->Where.Feet, feet );
            const char* way   = probed->Where.Feet.Y >= feet.Y ? "lifted" : "lowered";
            switch ( probed->Case )
            {
                case EProbed::Under:
                    return landed(
                         *probed, ETeleportSpot::Lifted,
                         std::format( "capsule under the ground, lifted {:.0f} cm onto its surface", moved ) );
                case EProbed::Encroached:
                    return landed( *probed, ETeleportSpot::Lifted,
                                   std::format( "capsule in the floor, {} {:.0f} cm onto it", way, moved ) );
                case EProbed::Snapped:
                    return landed( *probed, ETeleportSpot::Free,
                                   std::format( "point free, feet {} {:.0f} cm onto the floor", way, moved ) );
                case EProbed::Falling:
                    return landed( *probed, ETeleportSpot::Falling,
                                   std::format( "point free, floor {:.0f} cm below it: the pawn falls to it",
                                                probed->FloorBelowCm ) );
            }
        }

        // UE FindTeleportSpot's lateral tries: the ring at two radii around the point, at its height.
        const float ringCm = 2.0f * capsule.Radius;
        for ( int i = 0; i < ring; ++i )
        {
            const float angle =
                 2.0f * std::numbers::pi_v<float> * static_cast<float>( i ) / static_cast<float>( ring );
            const NetVec3 point{ feet.X + ringCm * std::cos( angle ), feet.Y,
                                 feet.Z + ringCm * std::sin( angle ) };
            if ( const std::optional<Probed> probed = Probe( point, capsule, settings, world, search ).Stood )
                return landed( *probed, ETeleportSpot::Shifted,
                               std::format( "point {}; shifted {:.0f} cm to ring probe {} of {}{}", centre.Why,
                                            DistanceCm( probed->Where.Feet, feet ), i + 1, ring,
                                            probed->Where.Grounded ? "" : ", falling to its floor" ) );
        }

        return Common::MakeError<TeleportSpot>(
             std::format( "no spot for the pawn at {}: the point {}; none of {} ring probe(s) stood either",
                          PointText( feet ), centre.Why, ring ) );
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
