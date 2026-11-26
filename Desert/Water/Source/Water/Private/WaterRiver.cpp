#include <Water/Public/WaterRiver.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace Desert::Water
{
    namespace
    {
        // The four control points of the segment @p t lies in, the ends clamped; @p u = t's fraction in it.
        struct Segment
        {
            glm::vec3 P0, P1, P2, P3;
            float     U = 0.0f;
        };

        Segment SegmentAt( const std::span<const glm::vec3> points, const float t ) noexcept
        {
            const auto  last  = static_cast<int32_t>( points.size() ) - 1;
            const float clamp = std::clamp( t, 0.0f, static_cast<float>( last ) );
            const auto  i     = std::min( static_cast<int32_t>( std::floor( clamp ) ), last - 1 );
            const auto  at    = [&]( const int32_t k )
            { return points[static_cast<size_t>( std::clamp( k, 0, last ) )]; };
            return Segment{ at( i - 1 ), at( i ), at( i + 1 ), at( i + 2 ), clamp - static_cast<float>( i ) };
        }

        // Samples per spline segment for the footprint and the bake's polyline.
        constexpr int32_t kSamplesPerSegment = 16;

        struct Sample
        {
            glm::vec2 Position{ 0.0f };
            glm::vec2 Direction{ 0.0f }; // unit, XZ
            float     Distance = 0.0f;   // along the spline, cm
            float     Y        = 0.0f;
        };

        std::vector<Sample> SampleSpline( const std::span<const glm::vec3> points ) noexcept
        {
            std::vector<Sample> samples;
            const auto          segments = static_cast<int32_t>( points.size() ) - 1;
            const int32_t       count    = segments * kSamplesPerSegment + 1;
            samples.reserve( static_cast<size_t>( count ) );
            float distance = 0.0f;
            for ( int32_t s = 0; s < count; ++s )
            {
                const float     t = static_cast<float>( s ) / static_cast<float>( kSamplesPerSegment );
                const glm::vec3 p = RiverSplinePoint( points, t );
                const glm::vec3 d = RiverSplineTangent( points, t );
                Sample          sample;
                sample.Position  = glm::vec2( p.x, p.z );
                sample.Y         = p.y;
                const float len  = glm::length( glm::vec2( d.x, d.z ) );
                sample.Direction = len > 1e-6f ? glm::vec2( d.x, d.z ) / len : glm::vec2( 0.0f );
                if ( !samples.empty() )
                    distance += glm::distance( samples.back().Position, sample.Position );
                sample.Distance = distance;
                samples.push_back( sample );
            }
            return samples;
        }

        // Whether the stretch samples[s] -> samples[s + 1] falls rather than flows: it drops steeper than
        // kRiverFallMinSlope (rise over run).
        bool SteepStretch( const std::vector<Sample>& samples, const size_t s ) noexcept
        {
            const float rise = samples[s].Y - samples[s + 1].Y;
            const float run  = glm::distance( samples[s].Position, samples[s + 1].Position );
            return rise > 0.0f && rise > kRiverFallMinSlope * run;
        }

        // The steep runs of the spline's stretches: [first, last] stretch indices of each run dropping at least
        // kRiverFallMinDropCm, upstream first.
        std::vector<std::pair<size_t, size_t>> SteepRuns( const std::vector<Sample>& samples ) noexcept
        {
            std::vector<std::pair<size_t, size_t>> runs;
            size_t                                 s = 0;
            while ( s + 1 < samples.size() )
            {
                if ( !SteepStretch( samples, s ) )
                {
                    ++s;
                    continue;
                }
                size_t last = s;
                while ( last + 2 < samples.size() && SteepStretch( samples, last + 1 ) )
                    ++last;
                if ( samples[s].Y - samples[last + 1].Y >= kRiverFallMinDropCm )
                    runs.emplace_back( s, last );
                s = last + 1;
            }
            return runs;
        }

        // THE VELOCITY FROM THE DROP: the water leaves the lip at the river's @p flowSpeed (cm/s) and gains by
        // falling, v^2 = v0^2 + 2 g dh (@p drop cm below the lip, @p gravity cm/s^2: the scene's). A slide on the bed
        // is frictionless, so the same rule holds on a chute.
        float RiverFallSpeed( const float flowSpeed, const float drop, const float gravity ) noexcept
        {
            return std::sqrt( flowSpeed * flowSpeed + 2.0f * std::max( gravity, 0.0f ) * std::max( drop, 0.0f ) );
        }

        // The march's horizontal step (cm) and its bound (a fall flies at most this far, cm).
        constexpr float kFlightStepCm   = 5.0f;
        constexpr float kFlightMaxRunCm = 100000.0f;

        // One point of the march: flight time, horizontal distance from the lip, height, velocity.
        struct FlightPoint
        {
            float T = 0.0f;
            float S = 0.0f;
            float Y = 0.0f;
            float Vh = 0.0f; // horizontal speed along the river's XZ path, cm/s
            float Vy = 0.0f; // vertical, cm/s (< 0 falling)
        };

        // The river's XZ path from a steep run's lip: the spline's samples over the run (bed heights known), then
        // straight on along the run's last direction (no bed: the level below is water, the flight plunges in).
        struct BedPath
        {
            std::vector<glm::vec2> Xz;
            std::vector<float>     S; // horizontal distance from the lip
            std::vector<float>     Y; // the bed (the spline's height)
            glm::vec2              Out{ 1.0f, 0.0f };

            glm::vec2 XzAt( const float s ) const noexcept
            {
                if ( s >= S.back() )
                    return Xz.back() + Out * ( s - S.back() );
                const size_t k = static_cast<size_t>( std::upper_bound( S.begin(), S.end(), s ) - S.begin() );
                const size_t a = k > 0 ? k - 1 : 0;
                const float  l = S[a + 1] - S[a];
                return glm::mix( Xz[a], Xz[a + 1], l > 0.0f ? ( s - S[a] ) / l : 0.0f );
            }
            glm::vec2 DirectionAt( const float s ) const noexcept
            {
                if ( s >= S.back() )
                    return Out;
                const size_t k = static_cast<size_t>( std::upper_bound( S.begin(), S.end(), s ) - S.begin() );
                const size_t a = k > 0 ? k - 1 : 0;
                const glm::vec2 d = Xz[a + 1] - Xz[a];
                const float     l = glm::length( d );
                return l > 1e-6f ? d / l : Out;
            }
            // The bed's height and slope (dy/ds) at @p s; nullopt past the run (no bed).
            std::optional<glm::vec2> BedAt( const float s ) const noexcept
            {
                if ( s > S.back() )
                    return std::nullopt;
                const size_t k = static_cast<size_t>( std::upper_bound( S.begin(), S.end(), s ) - S.begin() );
                const size_t a = std::min( k > 0 ? k - 1 : 0, S.size() - 2 );
                const float  l = S[a + 1] - S[a];
                if ( !( l > 1e-3f ) )
                    return glm::vec2( std::min( Y[a], Y[a + 1] ), -1e6f ); // a vertical stretch: a wall
                return glm::vec2( glm::mix( Y[a], Y[a + 1], ( s - S[a] ) / l ), ( Y[a + 1] - Y[a] ) / l );
            }
        };

        // A steep run's fall: the water's flight from the lip (horizontal at flowSpeed, under gravity, sliding on
        // the bed where the bed stands above the flight) down to kRiverFallPlungeCm under the level below the run,
        // sampled kRiverFallSheetSamples times uniformly in flight time after one flat sample of overlap on the
        // river above the lip; then its plunge where the flight meets the level below.
        RiverFall MakeFall( const std::vector<Sample>& samples, const std::pair<size_t, size_t> run,
                            const float width, const float flowSpeed, const float gravity ) noexcept
        {
            RiverFall     fall;
            const Sample& lip        = samples[run.first];
            const float   lipY       = lip.Y;
            const float   levelBelow = samples[run.second + 1].Y;
            const float   v0         = std::max( flowSpeed, 0.0f );
            fall.Width               = width;
            fall.FlowSpeed           = v0;
            fall.Lip                 = glm::vec3( lip.Position.x, lipY, lip.Position.y );
            fall.Height              = lipY - levelBelow;
            // Across the sheet: the river's width direction at the lip (its XZ direction turned a quarter).
            const glm::vec2 d = lip.Direction;
            fall.Across = glm::length( d ) > 1e-6f ? glm::vec3( -d.y, 0.0f, d.x ) : glm::vec3( 1.0f, 0.0f, 0.0f );

            BedPath bed;
            for ( size_t k = run.first; k <= run.second + 1; ++k )
            {
                bed.S.push_back( bed.Xz.empty() ? 0.0f
                                                : bed.S.back() + glm::distance( bed.Xz.back(), samples[k].Position ) );
                bed.Xz.push_back( samples[k].Position );
                bed.Y.push_back( samples[k].Y );
            }
            const glm::vec2 out = samples[run.second + 1].Direction;
            bed.Out = glm::length( out ) > 1e-6f ? out : ( glm::length( d ) > 1e-6f ? d : glm::vec2( 1.0f, 0.0f ) );

            // THE MARCH: one kFlightStepCm of path per step (free flight, or the bed's slope while sliding).
            std::vector<FlightPoint> path;
            FlightPoint              p{ .T = 0.0f, .S = 0.0f, .Y = lipY, .Vh = v0, .Vy = 0.0f };
            path.push_back( p );
            const float end = levelBelow - kRiverFallPlungeCm;
            while ( p.Y > end && p.S < kFlightMaxRunCm )
            {
                const float speed = std::sqrt( p.Vh * p.Vh + p.Vy * p.Vy );
                const float dt    = kFlightStepCm / std::max( speed, 1.0f );
                p.S += p.Vh * dt;
                p.Y += p.Vy * dt - 0.5f * gravity * dt * dt;
                p.Vy -= gravity * dt;
                p.T += dt;
                if ( const std::optional<glm::vec2> b = bed.BedAt( p.S ); b && b->x > p.Y )
                {
                    // On the bed: the water slides down it at the speed its drop gives, along its slope.
                    p.Y               = b->x;
                    const float v     = RiverFallSpeed( v0, lipY - p.Y, gravity );
                    const float slope = b->y;
                    const float n     = std::sqrt( 1.0f + slope * slope );
                    p.Vh              = v / n;
                    p.Vy              = v * slope / n;
                }
                path.push_back( p );
            }

            // The flight at time @p t (linear between the march's points).
            const auto at = [&path]( const float t ) noexcept
            {
                const auto it = std::lower_bound( path.begin(), path.end(), t,
                                                  []( const FlightPoint& a, const float v ) { return a.T < v; } );
                if ( it == path.begin() )
                    return path.front();
                if ( it == path.end() )
                    return path.back();
                const FlightPoint& a = *( it - 1 );
                const FlightPoint& b = *it;
                const float        f = b.T > a.T ? ( t - a.T ) / ( b.T - a.T ) : 0.0f;
                return FlightPoint{ .T  = t,
                                    .S  = glm::mix( a.S, b.S, f ),
                                    .Y  = glm::mix( a.Y, b.Y, f ),
                                    .Vh = glm::mix( a.Vh, b.Vh, f ),
                                    .Vy = glm::mix( a.Vy, b.Vy, f ) };
            };
            const auto sampleOf = [&]( const FlightPoint& f ) noexcept
            {
                const glm::vec2 xz  = bed.XzAt( f.S );
                const glm::vec2 dir = bed.DirectionAt( f.S );
                RiverFallSample sample;
                sample.Position     = glm::vec3( xz.x, f.Y, xz.y );
                const glm::vec3 v   = glm::vec3( dir.x * f.Vh, f.Vy, dir.y * f.Vh );
                const float     len = glm::length( v );
                sample.Direction    = len > 1e-6f ? v / len : glm::vec3( 0.0f, -1.0f, 0.0f );
                sample.Drop         = std::max( lipY - f.Y, 0.0f );
                sample.Speed        = RiverFallSpeed( v0, sample.Drop, gravity );
                return sample;
            };

            // The overlap sample on the river above the lip (the spline's, or a texel upstream along the flow).
            RiverFallSample overlap;
            if ( run.first > 0 )
            {
                const Sample& up     = samples[run.first - 1];
                overlap.Position     = glm::vec3( up.Position.x, up.Y, up.Position.y );
                fall.LipFlowDistance = up.Distance;
            }
            else
            {
                const glm::vec2 xz   = lip.Position - bed.DirectionAt( 0.0f ) * kRiverFlowCmPerTexel;
                overlap.Position     = glm::vec3( xz.x, lipY, xz.y );
                fall.LipFlowDistance = lip.Distance - kRiverFlowCmPerTexel;
            }
            const glm::vec3 toLip = fall.Lip - overlap.Position;
            overlap.Direction     = glm::length( toLip ) > 1e-6f ? glm::normalize( toLip ) : glm::vec3( 1, 0, 0 );
            overlap.Speed         = v0;
            fall.Samples.push_back( overlap );
            const float tEnd = path.back().T;
            for ( uint32_t k = 0; k < kRiverFallSheetSamples; ++k )
                fall.Samples.push_back( sampleOf(
                     at( tEnd * static_cast<float>( k ) / static_cast<float>( kRiverFallSheetSamples - 1u ) ) ) );
            for ( size_t k = 1; k < fall.Samples.size(); ++k )
                fall.Samples[k].Distance = fall.Samples[k - 1].Distance +
                                           glm::distance( fall.Samples[k - 1].Position, fall.Samples[k].Position );

            // THE PLUNGE: where the flight crosses the level below.
            FlightPoint landing = path.back();
            for ( size_t k = 1; k < path.size(); ++k )
                if ( path[k].Y <= levelBelow )
                {
                    const FlightPoint& a = path[k - 1];
                    const FlightPoint& b = path[k];
                    landing = at( glm::mix( a.T, b.T, b.Y < a.Y ? ( a.Y - levelBelow ) / ( a.Y - b.Y ) : 0.0f ) );
                    break;
                }
            const glm::vec2 footXz       = bed.XzAt( landing.S );
            fall.Foot                    = glm::vec3( footXz.x, levelBelow, footXz.y );
            fall.Plunge.Foot             = fall.Foot;
            fall.Plunge.Across           = fall.Across;
            fall.Plunge.Downstream       = bed.DirectionAt( landing.S );
            fall.Plunge.HalfWidth        = 0.5f * width;
            fall.Plunge.Radius           = fall.Plunge.HalfWidth + kPlungeRadiusPerDrop * fall.Height;
            fall.Plunge.LandingSpeed     = RiverFallSpeed( v0, fall.Height, gravity );
            fall.Plunge.OutflowSpeed     = kPlungeOutflowShare * fall.Plunge.LandingSpeed;
            return fall;
        }

        // Whether @p xz lies under a fall's flight (between its lip and its plunge, within its half width).
        bool UnderFlight( const RiverFall& fall, const glm::vec2 xz ) noexcept
        {
            const glm::vec2 across = glm::vec2( fall.Across.x, fall.Across.z );
            for ( size_t k = 1; k + 1 < fall.Samples.size(); ++k )
            {
                const glm::vec2 a  = glm::vec2( fall.Samples[k].Position.x, fall.Samples[k].Position.z );
                const glm::vec2 b  = glm::vec2( fall.Samples[k + 1].Position.x, fall.Samples[k + 1].Position.z );
                const glm::vec2 ab = b - a;
                const float     l2 = glm::dot( ab, ab );
                const float     u  = l2 > 0.0f ? glm::dot( xz - a, ab ) / l2 : 0.0f;
                if ( u < 0.0f || u > 1.0f )
                    continue;
                if ( std::abs( glm::dot( xz - ( a + u * ab ), across ) ) < 0.5f * fall.Width )
                    return true;
            }
            return false;
        }

        // The swept ribbon's bounds over samples [0, count): half the width out from every sample.
        RiverFootprint FootprintOf( const std::vector<Sample>& samples, const size_t count, const float width ) noexcept
        {
            RiverFootprint box{ .Min = glm::vec2( 1.0f ), .Max = glm::vec2( 0.0f ) };
            if ( count == 0 || !( width > 0.0f ) )
                return box;
            box.Min = glm::vec2( std::numeric_limits<float>::max() );
            box.Max = glm::vec2( std::numeric_limits<float>::lowest() );
            for ( size_t s = 0; s < count; ++s )
            {
                box.Min = glm::min( box.Min, samples[s].Position );
                box.Max = glm::max( box.Max, samples[s].Position );
            }
            box.Min -= glm::vec2( 0.5f * width );
            box.Max += glm::vec2( 0.5f * width );
            return box;
        }

        // THE MOUTH: the first sample at or downstream of the last fall's foot (of the spline's start with no
        // fall) inside a receiver's footprint at or below its level + kRiverMouthToleranceCm.
        std::optional<std::pair<size_t, RiverMouth>> FindMouth( const std::vector<Sample>&             samples,
                                                                const std::vector<std::pair<size_t, size_t>>& runs,
                                                                const std::vector<RiverFall>&           falls,
                                                                const std::span<const RiverReceiver>    receivers,
                                                                const float width ) noexcept
        {
            const size_t first = runs.empty() ? 1 : runs.back().second + 1;
            for ( size_t s = first; s < samples.size(); ++s )
                for ( const RiverReceiver& receiver : receivers )
                {
                    const glm::vec2 p = samples[s].Position;
                    if ( p.x < receiver.Min.x || p.y < receiver.Min.y || p.x > receiver.Max.x || p.y > receiver.Max.y ||
                         samples[s].Y > receiver.Level + kRiverMouthToleranceCm )
                        continue;
                    RiverMouth mouth{ .Into = receiver.Id, .Point = p, .Radius = width };
                    if ( !falls.empty() )
                    {
                        const FallPlunge& plunge = falls.back().Plunge;
                        const glm::vec2   foot   = glm::vec2( plunge.Foot.x, plunge.Foot.z );
                        mouth.Radius = glm::dot( p - foot, plunge.Downstream ) <= 0.0f
                                            ? 0.0f
                                            : std::min( width, glm::distance( p, foot ) );
                    }
                    return std::make_pair( s, mouth );
                }
            return std::nullopt;
        }
    } // namespace

    std::optional<RiverFallHit> RiverFallAt( const RiverFlowField& field, const glm::vec3& point ) noexcept
    {
        std::optional<RiverFallHit> best;
        float                       bestDistance = 0.5f * kRiverFallSheetThicknessCm;
        for ( const RiverFall& fall : field.Falls )
            for ( size_t k = 0; k + 1 < fall.Samples.size(); ++k )
            {
                const RiverFallSample& a   = fall.Samples[k];
                const RiverFallSample& b   = fall.Samples[k + 1];
                const glm::vec3        ab  = b.Position - a.Position;
                const float            ab2 = glm::dot( ab, ab );
                if ( !( ab2 > 0.0f ) )
                    continue;
                // The point's place along the stretch and across the sheet: outside either, not this stretch.
                const float u = glm::dot( point - a.Position, ab ) / ab2;
                if ( u < 0.0f || u > 1.0f )
                    continue;
                const glm::vec3 centre = a.Position + u * ab;
                const float     across = glm::dot( point - centre, fall.Across );
                if ( std::abs( across ) > 0.5f * fall.Width )
                    continue;
                // The sheet's normal (across x down the sheet), turned to the point's side.
                glm::vec3   normal = glm::normalize( glm::cross( fall.Across, ab ) );
                const float off    = glm::dot( point - centre, normal );
                if ( std::abs( off ) >= bestDistance )
                    continue;
                bestDistance = std::abs( off );
                if ( off < 0.0f )
                    normal = -normal;
                best = RiverFallHit{ .SheetPoint = centre + across * fall.Across,
                                     .Normal     = normal,
                                     .Velocity   = glm::normalize( ab ) * glm::mix( a.Speed, b.Speed, u ),
                                     .Immersion  = 0.5f * kRiverFallSheetThicknessCm - std::abs( off ) };
            }
        return best;
    }

    glm::vec2 PlungeVelocityAt( const FallPlunge& plunge, const glm::vec2 xz ) noexcept
    {
        const glm::vec2 foot = glm::vec2( plunge.Foot.x, plunge.Foot.z );
        const float     d    = glm::distance( xz, foot );
        if ( !( plunge.Radius > 0.0f ) || d >= plunge.Radius )
            return glm::vec2( 0.0f );
        // Away from the landing line (the segment Foot +- Across * HalfWidth); downstream on the line itself.
        const glm::vec2 across = glm::vec2( plunge.Across.x, plunge.Across.z );
        const float     along  = std::clamp( glm::dot( xz - foot, across ), -plunge.HalfWidth, plunge.HalfWidth );
        const glm::vec2 away   = xz - ( foot + along * across );
        const float     l      = glm::length( away );
        const glm::vec2 dir    = l > 1e-3f ? away / l : plunge.Downstream;
        const float     fade   = 1.0f - d / plunge.Radius;
        return dir * ( plunge.OutflowSpeed * fade * fade );
    }

    float RiverMouthWeight( const RiverFlowField& field, const glm::vec2 xz ) noexcept
    {
        if ( !field.Mouth || !( field.Mouth->Radius > 0.0f ) )
            return 0.0f;
        return std::clamp( 1.0f - glm::distance( xz, field.Mouth->Point ) / field.Mouth->Radius, 0.0f, 1.0f );
    }

    // UE: USplineComponent's default CurveMode is cubic Hermite with Catmull-Rom (auto) tangents - the same curve.
    glm::vec3 RiverSplinePoint( const std::span<const glm::vec3> points, const float t ) noexcept
    {
        if ( points.empty() )
            return glm::vec3( 0.0f );
        if ( points.size() == 1 )
            return points[0];
        const Segment s  = SegmentAt( points, t );
        const float   u  = s.U;
        const float   u2 = u * u;
        const float   u3 = u2 * u;
        return 0.5f *
               ( 2.0f * s.P1 + ( s.P2 - s.P0 ) * u + ( 2.0f * s.P0 - 5.0f * s.P1 + 4.0f * s.P2 - s.P3 ) * u2 +
                 ( -s.P0 + 3.0f * s.P1 - 3.0f * s.P2 + s.P3 ) * u3 );
    }

    glm::vec3 RiverSplineTangent( const std::span<const glm::vec3> points, const float t ) noexcept
    {
        if ( points.size() < 2 )
            return glm::vec3( 0.0f );
        const Segment s  = SegmentAt( points, t );
        const float   u  = s.U;
        const float   u2 = u * u;
        return 0.5f * ( ( s.P2 - s.P0 ) + 2.0f * ( 2.0f * s.P0 - 5.0f * s.P1 + 4.0f * s.P2 - s.P3 ) * u +
                        3.0f * ( -s.P0 + 3.0f * s.P1 - 3.0f * s.P2 + s.P3 ) * u2 );
    }

    RiverFootprint ComputeRiverFootprint( const std::span<const glm::vec3> points, const float width ) noexcept
    {
        if ( points.size() < 2 )
            return RiverFootprint{ .Min = glm::vec2( 1.0f ), .Max = glm::vec2( 0.0f ) };
        const std::vector<Sample> samples = SampleSpline( points );
        return FootprintOf( samples, samples.size(), width );
    }

    glm::vec3 SampleRiverFlowBilinear( const RiverFlowField& field, const glm::vec2 xz ) noexcept
    {
        if ( field.Size.x == 0u || field.Size.y == 0u ||
             field.Texels.size() != static_cast<size_t>( field.Size.x ) * field.Size.y )
            return glm::vec3( 0.0f );
        // The texel space the GPU reads in (WaterRiverTexelSpace): texel centres on integers.
        const glm::vec2 p   = ( xz - field.Min ) / field.Extent * glm::vec2( field.Size ) - 0.5f;
        const glm::vec2 lo  = glm::floor( p );
        const glm::vec2 f   = p - lo;
        glm::vec3       sum = glm::vec3( 0.0f );
        for ( int dz = 0; dz < 2; ++dz )
            for ( int dx = 0; dx < 2; ++dx )
            {
                const int64_t x = static_cast<int64_t>( lo.x ) + dx;
                const int64_t z = static_cast<int64_t>( lo.y ) + dz;
                if ( x < 0 || z < 0 || x >= int64_t{ field.Size.x } || z >= int64_t{ field.Size.y } )
                    continue; // off the field: zero, as WaterRiverTexel returns off the region
                const glm::vec4 t =
                     field.Texels[static_cast<size_t>( z ) * field.Size.x + static_cast<size_t>( x )];
                const float w = ( dx == 1 ? f.x : 1.0f - f.x ) * ( dz == 1 ? f.y : 1.0f - f.y );
                sum += w * glm::vec3( t );
            }
        return sum;
    }

    bool RiverInsideBank( const RiverFlowField& field, const glm::vec2 xz ) noexcept
    {
        return SampleRiverFlowBilinear( field, xz ).z >= kRiverBankClip;
    }

    std::optional<float> SampleRiverSurfaceY( const RiverFlowField& field, const glm::vec2 xz ) noexcept
    {
        if ( field.Size.x == 0u || field.Size.y == 0u || field.SurfaceHeightCm.size() != field.Texels.size() )
            return std::nullopt;
        const glm::vec2 p      = ( xz - field.Min ) / field.Extent * glm::vec2( field.Size ) - 0.5f;
        const glm::vec2 lo     = glm::floor( p );
        const glm::vec2 f      = p - lo;
        float           sum    = 0.0f;
        float           weight = 0.0f;
        for ( int dz = 0; dz < 2; ++dz )
            for ( int dx = 0; dx < 2; ++dx )
            {
                const int64_t x = static_cast<int64_t>( lo.x ) + dx;
                const int64_t z = static_cast<int64_t>( lo.y ) + dz;
                if ( x < 0 || z < 0 || x >= int64_t{ field.Size.x } || z >= int64_t{ field.Size.y } )
                    continue;
                const size_t i = static_cast<size_t>( z ) * field.Size.x + static_cast<size_t>( x );
                if ( field.Texels[i].z <= 0.5f )
                    continue;
                const float w = ( dx == 1 ? f.x : 1.0f - f.x ) * ( dz == 1 ? f.y : 1.0f - f.y );
                sum += w * field.SurfaceHeightCm[i];
                weight += w;
            }
        if ( !( weight > 0.0f ) )
            return std::nullopt;
        return sum / weight;
    }

    RiverFlowField BakeRiverFlow( const std::span<const glm::vec3> points, const float width,
                                  const float flowSpeed, const float flowPeriod, const float cmPerTexel,
                                  const float gravity, const std::span<const RiverReceiver> receivers ) noexcept
    {
        RiverFlowField field;
        field.FlowPeriod = flowPeriod > 0.0f ? flowPeriod : 1.0f;
        if ( points.size() < 2 || !( width > 0.0f ) )
            return field;
        const std::vector<Sample> samples = SampleSpline( points );
        // THE FALLS (each steep run of stretches flown as a sheet, RiverFall) and THE MOUTH: the field stops at the
        // mouth, the receiver owns the water beyond it (its tiles, its waves, its queries).
        const std::vector<std::pair<size_t, size_t>> runs = SteepRuns( samples );
        for ( const std::pair<size_t, size_t>& run : runs )
            field.Falls.push_back( MakeFall( samples, run, width, flowSpeed, gravity ) );
        size_t count = samples.size();
        if ( const auto mouth = FindMouth( samples, runs, field.Falls, receivers, width ) )
        {
            field.Mouth = mouth->second;
            count       = mouth->first + 1;
        }
        const RiverFootprint box = FootprintOf( samples, count, width );
        if ( !( box.Min.x < box.Max.x ) || !( box.Min.y < box.Max.y ) )
            return field;

        const glm::vec2 extent  = box.Max - box.Min;
        const float     longest = std::max( extent.x, extent.y );
        const float     texel   = std::max( cmPerTexel > 0.0f ? cmPerTexel : 100.0f,
                                      longest / static_cast<float>( kRiverFlowMaxSize ) );
        field.Min    = box.Min;
        field.Extent = extent;
        field.Size   = glm::uvec2(
             std::clamp( static_cast<uint32_t>( std::ceil( extent.x / texel ) ), 1u, kRiverFlowMaxSize ),
             std::clamp( static_cast<uint32_t>( std::ceil( extent.y / texel ) ), 1u, kRiverFlowMaxSize ) );
        field.Texels.assign( static_cast<size_t>( field.Size.x ) * field.Size.y, glm::vec4( 0.0f ) );
        field.SurfaceHeightCm.assign( field.Texels.size(), 0.0f );

        // The nearest polyline segment per texel, rasterised segment by segment over the texels its swept box
        // touches (cost ~ the ribbon's area, not texels x samples), up to the mouth.
        const float               halfWidth = 0.5f * width;
        const glm::vec2           step      = extent / glm::vec2( field.Size );
        std::vector<float>        nearest( field.Texels.size(), std::numeric_limits<float>::max() );
        // The stretch each texel took its values from: a texel nearest a steep stretch lies under a fall's sheet.
        std::vector<uint32_t> owner( field.Texels.size(), std::numeric_limits<uint32_t>::max() );
        double                sumY = 0.0;
        for ( size_t s = 0; s < count; ++s )
            sumY += samples[s].Y;
        field.SurfaceY = static_cast<float>( sumY / static_cast<double>( count ) );

        for ( size_t s = 0; s + 1 < count; ++s )
        {
            const Sample&   a   = samples[s];
            const Sample&   b   = samples[s + 1];
            const glm::vec2 ab  = b.Position - a.Position;
            const float     ab2 = glm::dot( ab, ab );
            const glm::vec2 lo  = ( glm::min( a.Position, b.Position ) - halfWidth - field.Min ) / step;
            const glm::vec2 hi  = ( glm::max( a.Position, b.Position ) + halfWidth - field.Min ) / step;
            const auto      x0  = static_cast<uint32_t>( std::max( 0.0f, std::floor( lo.x ) ) );
            const auto      z0  = static_cast<uint32_t>( std::max( 0.0f, std::floor( lo.y ) ) );
            const uint32_t  x1 =
                 std::min( field.Size.x, static_cast<uint32_t>( std::max( 0.0f, std::ceil( hi.x ) ) ) );
            const uint32_t z1 =
                 std::min( field.Size.y, static_cast<uint32_t>( std::max( 0.0f, std::ceil( hi.y ) ) ) );
            for ( uint32_t z = z0; z < z1; ++z )
                for ( uint32_t x = x0; x < x1; ++x )
                {
                    const glm::vec2 centre = field.Min + ( glm::vec2( x, z ) + 0.5f ) * step;
                    const float     u =
                         ab2 > 0.0f ? std::clamp( glm::dot( centre - a.Position, ab ) / ab2, 0.0f, 1.0f ) : 0.0f;
                    const float  d = glm::distance( centre, a.Position + u * ab );
                    const size_t i = static_cast<size_t>( z ) * field.Size.x + x;
                    if ( d >= halfWidth || d >= nearest[i] )
                        continue;
                    nearest[i]            = d;
                    owner[i]              = static_cast<uint32_t>( s );
                    glm::vec2   direction = glm::mix( a.Direction, b.Direction, u );
                    const float len       = glm::length( direction );
                    direction             = len > 1e-6f ? direction / len : glm::vec2( 0.0f );
                    // A parabolic cross-section profile: full speed on the centre line, still water at the bank.
                    const float across = d / halfWidth;
                    const float ease   = 1.0f - across * across;
                    field.Texels[i]    = glm::vec4( direction * ( flowSpeed * ease ), 1.0f,
                                                    glm::mix( a.Distance, b.Distance, u ) );
                    // The level falls along the flow: the nearest spline point's height (UE: the spline's Z at
                    // the closest input key), flat across the ribbon.
                    field.SurfaceHeightCm[i] = glm::mix( a.Y, b.Y, u );
                }
        }
        // Under a fall, no level surface - no bank mask, no level - so neither the quadtree's river tiles nor a
        // CPU query lay a flat surface across the drop: the texels a steep stretch owns and the texels under the
        // flight are the sheet's (WaterRenderer's fall rows draw it, RiverFallAt answers it).
        if ( !runs.empty() )
            for ( size_t i = 0; i < field.Texels.size(); ++i )
            {
                const glm::vec2 centre =
                     field.Min + ( glm::vec2( static_cast<float>( i % field.Size.x ), static_cast<float>( i / field.Size.x ) ) +
                                  0.5f ) *
                                    step;
                for ( size_t r = 0; r < runs.size(); ++r )
                    if ( ( owner[i] >= runs[r].first && owner[i] <= runs[r].second ) ||
                         UnderFlight( field.Falls[r], centre ) )
                    {
                        field.Texels[i]          = glm::vec4( 0.0f );
                        field.SurfaceHeightCm[i] = 0.0f;
                        break;
                    }
            }

        field.MinSurfaceY = std::numeric_limits<float>::max();
        field.MaxSurfaceY = std::numeric_limits<float>::lowest();
        for ( size_t i = 0; i < field.Texels.size(); ++i )
            if ( field.Texels[i].z > 0.5f )
            {
                field.MinSurfaceY = std::min( field.MinSurfaceY, field.SurfaceHeightCm[i] );
                field.MaxSurfaceY = std::max( field.MaxSurfaceY, field.SurfaceHeightCm[i] );
            }
        if ( field.MinSurfaceY > field.MaxSurfaceY )
            field.MinSurfaceY = field.MaxSurfaceY = field.SurfaceY; // a ribbon thinner than one texel centre
        return field;
    }

    uint64_t RiverBakeKey( const RiverBakeInput& input ) noexcept
    {
        uint64_t   key = 1469598103934665603ull;
        const auto mix = [&key]( const float v )
        { key = ( key ^ std::bit_cast<uint32_t>( v ) ) * 1099511628211ull; };
        mix( input.Origin.x );
        mix( input.Origin.y );
        mix( input.Origin.z );
        mix( input.Width );
        mix( input.FlowSpeed );
        mix( input.FlowPeriod );
        mix( input.Gravity );
        key = ( key ^ input.Receivers.size() ) * 1099511628211ull;
        for ( const RiverReceiver& receiver : input.Receivers )
        {
            key = ( key ^ receiver.Id ) * 1099511628211ull;
            mix( receiver.Min.x );
            mix( receiver.Min.y );
            mix( receiver.Max.x );
            mix( receiver.Max.y );
            mix( receiver.Level );
        }
        key = ( key ^ input.LocalPoints.size() ) * 1099511628211ull;
        for ( const glm::vec3& p : input.LocalPoints )
        {
            mix( p.x );
            mix( p.y );
            mix( p.z );
        }
        return key;
    }

    std::shared_ptr<const RiverFlowField> BakeRiver( const RiverBakeInput& input ) noexcept
    {
        std::vector<glm::vec3> world( input.LocalPoints.begin(), input.LocalPoints.end() );
        for ( glm::vec3& p : world )
            p += input.Origin;
        return std::make_shared<const RiverFlowField>(
             BakeRiverFlow( world, input.Width, input.FlowSpeed, input.FlowPeriod, kRiverFlowCmPerTexel,
                            input.Gravity, input.Receivers ) );
    }
} // namespace Desert::Water
