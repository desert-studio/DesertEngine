#include <Engine/Gameplay/TeleportSpot.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
        //  Under: ONE upward ray from the point, back faces met (SurfaceAbove), by ray parity per body: a front
        //    face enters its body, a back face of a body the ray entered leaves it, and the first back face of a
        //    body the ray has NOT entered says the point is under or inside that body (a heightfield, a mesh
        //    floor, a box) - a buried mesh or rock passed on the way up does not make the point free. Its top is a
        //    candidate, and so is every top above it (the back face of any body) while the walk lifts: front
        //    faces are entered and passed. The feet stand on a candidate only when the capsule fits there AND the
        //    same parity walk from the stood feet meets no back face of a body it has not entered: a landed pawn
        //    is never under a one-sided surface, and never on a roof it was not inside. A candidate the capsule
        //    does not fit on is passed and the walk goes on from the skin above it, up to LiftCm; no stand there,
        //    or more bodies entered at once than the walk holds: refused.
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

        // The bodies whose front face one upward walk met and whose back face it has not met yet: the bodies the
        // ray is inside of now (ray parity per body). A fixed few - a ray inside more at once is refused by name.
        class EnteredBodies
        {
        public:
            static constexpr std::size_t kMax = 8;

            [[nodiscard]] bool Has( std::uint32_t body ) const
            {
                const auto end = m_Bodies.begin() + m_Count;
                return std::find( m_Bodies.begin(), end, body ) != end;
            }
            // False when the walk holds kMax bodies already.
            [[nodiscard]] bool Enter( std::uint32_t body )
            {
                if ( Has( body ) )
                    return true;
                if ( m_Count == kMax )
                    return false;
                m_Bodies[m_Count++] = body;
                return true;
            }
            void Leave( std::uint32_t body )
            {
                const auto end = m_Bodies.begin() + m_Count;
                const auto it  = std::find( m_Bodies.begin(), end, body );
                if ( it == end )
                    return;
                *it = m_Bodies[--m_Count];
            }

        private:
            std::array<std::uint32_t, kMax> m_Bodies{};
            std::size_t                     m_Count = 0;
        };

        enum class EWalk : std::uint8_t
        {
            Under, // @p hit is the back face of a body the walk had not entered: the start is under or inside it
            Free,  // up to the lift's top every body met was entered and left, or entered only (a ceiling)
            Full   // more bodies entered at once than EnteredBodies holds
        };
        // The parity walk up the column of @p point from @p hit (the first surface the ray from the start met) to
        // @p liftTop: front faces enter, back faces of entered bodies leave; stops at the back face of a body not
        // entered, @p hit left on it.
        [[nodiscard]] EWalk WalkToUnder( std::optional<SurfaceAbove>& hit, EnteredBodies& entered,
                                         const NetVec3& point, float liftTop, const IPawnMoveWorld& world,
                                         const TeleportSearch& search )
        {
            while ( hit )
            {
                if ( hit->Underside )
                {
                    if ( !entered.Enter( hit->Body ) )
                        return EWalk::Full;
                }
                else if ( entered.Has( hit->Body ) )
                    entered.Leave( hit->Body );
                else
                    return EWalk::Under;
                const float from = hit->Y + search.SkinCm;
                if ( from >= liftTop )
                    return EWalk::Free;
                hit = world.SurfaceAbove( Raised( point, from - point.Y ), liftTop - from );
            }
            return EWalk::Free;
        }

        [[nodiscard]] std::string FullWhy()
        {
            return std::format( "inside more than {} bodies at once on its upward ray", EnteredBodies::kMax );
        }

        [[nodiscard]] ProbeResult Probe( const NetVec3& point, const PawnCapsule& capsule,
                                         const PawnMoveSettings& settings, const IPawnMoveWorld& world,
                                         const TeleportSearch& search )
        {
            const float                 liftTop = point.Y + search.LiftCm;
            EnteredBodies               entered;
            std::optional<SurfaceAbove> hit   = world.SurfaceAbove( point, search.LiftCm );
            const EWalk                 first = WalkToUnder( hit, entered, point, liftTop, world, search );
            if ( first == EWalk::Full )
                return { std::nullopt, FullWhy() };
            if ( first == EWalk::Under )
            {
                const float depth = hit->Y - point.Y;
                while ( hit )
                {
                    if ( hit->Underside )
                    {
                        if ( !entered.Enter( hit->Body ) )
                            return { std::nullopt, FullWhy() };
                    }
                    else
                    {
                        entered.Leave( hit->Body ); // a top: the walk is out of its body here
                        if ( const std::optional<Stand> stand = StandOn( point, hit->Y, capsule, world, search ) )
                        {
                            const float                 upCm = liftTop - stand->Feet.Y;
                            EnteredBodies               overEntered;
                            std::optional<SurfaceAbove> over =
                                 upCm > 0.0f ? world.SurfaceAbove( stand->Feet, upCm ) : std::nullopt;
                            const EWalk above = WalkToUnder( over, overEntered, point, liftTop, world, search );
                            if ( above == EWalk::Full )
                                return { std::nullopt, FullWhy() };
                            if ( above == EWalk::Free )
                                return { Probed{ *stand, EProbed::Under, point.Y - hit->Y }, {} };
                            hit     = over; // still under a body: its top is the next candidate
                            entered = overEntered;
                            continue;
                        }
                    }
                    const float from = hit->Y + search.SkinCm; // the walk goes on above this surface
                    if ( from >= liftTop )
                        break;
                    hit = world.SurfaceAbove( Raised( point, from - point.Y ), liftTop - from );
                }
                return { std::nullopt, std::format( "under or inside a body {:.0f} cm below its top, and no top "
                                                    "within {:.0f} cm above it has room for the capsule",
                                                    depth, search.LiftCm ) };
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
            const bool  up    = probed->Where.Feet.Y > feet.Y;
            const char* way   = up ? "lifted" : "lowered";
            // Lifted is a raise onto the floor above (TeleportSpot.hpp); feet set down onto a floor are Free.
            const ETeleportSpot raised = up ? ETeleportSpot::Lifted : ETeleportSpot::Free;
            switch ( probed->Case )
            {
                case EProbed::Under:
                    return landed(
                         *probed, raised,
                         std::format( "capsule under or inside the ground, {} {:.0f} cm onto its surface", way,
                                      moved ) );
                case EProbed::Encroached:
                    return landed( *probed, raised,
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
