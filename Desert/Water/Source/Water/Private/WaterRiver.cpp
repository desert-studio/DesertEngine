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

        // A steep run's sheet: its samples plus one flat sample of overlap at each end (where the spline has
        // one), so the sheet starts on the river's surface above the lip and ends on the water below the foot.
        RiverFall MakeFall( const std::vector<Sample>& samples, const std::pair<size_t, size_t> run,
                            const float width, const float flowSpeed ) noexcept
        {
            RiverFall     fall;
            const size_t  first  = run.first > 0 ? run.first - 1 : run.first;
            const size_t  last   = std::min( run.second + 2, samples.size() - 1 );
            const float   lipY   = samples[run.first].Y;
            const Sample& foot   = samples[run.second + 1];
            fall.Width           = width;
            fall.FlowSpeed       = flowSpeed;
            fall.Lip             = glm::vec3( samples[run.first].Position.x, lipY, samples[run.first].Position.y );
            fall.Foot            = glm::vec3( foot.Position.x, foot.Y, foot.Position.y );
            fall.Height          = lipY - foot.Y;
            fall.LipFlowDistance = samples[first].Distance;
            // Across the sheet: the river's width direction at the lip (its XZ direction turned a quarter).
            const glm::vec2 d = samples[run.first].Direction;
            fall.Across = glm::length( d ) > 1e-6f ? glm::vec3( -d.y, 0.0f, d.x ) : glm::vec3( 1.0f, 0.0f, 0.0f );
            float distance = 0.0f;
            for ( size_t k = first; k <= last; ++k )
            {
                RiverFallSample sample;
                sample.Position = glm::vec3( samples[k].Position.x, samples[k].Y, samples[k].Position.y );
                if ( !fall.Samples.empty() )
                    distance += glm::distance( fall.Samples.back().Position, sample.Position );
                sample.Distance = distance;
                sample.Drop     = std::max( lipY - samples[k].Y, 0.0f );
                fall.Samples.push_back( sample );
            }
            // Each sample's direction: down the centre line (central difference, one-sided at the ends).
            for ( size_t k = 0; k < fall.Samples.size(); ++k )
            {
                const glm::vec3 a         = fall.Samples[k > 0 ? k - 1 : k].Position;
                const glm::vec3 b         = fall.Samples[std::min( k + 1, fall.Samples.size() - 1 )].Position;
                const float     l         = glm::distance( a, b );
                fall.Samples[k].Direction = l > 1e-6f ? ( b - a ) / l : glm::vec3( 0.0f, -1.0f, 0.0f );
            }
            return fall;
        }
    } // namespace

    float RiverFallSpeed( const float flowSpeed, const float drop, const float gravity ) noexcept
    {
        return std::sqrt( flowSpeed * flowSpeed + 2.0f * std::max( gravity, 0.0f ) * std::max( drop, 0.0f ) );
    }

    std::optional<RiverFallHit> RiverFallAt( const RiverFlowField& field, const glm::vec3& point,
                                             const float gravity ) noexcept
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
                const float drop = a.Drop + u * ( b.Drop - a.Drop );
                best             = RiverFallHit{ .SheetPoint = centre + across * fall.Across,
                                                 .Normal     = normal,
                                                 .Velocity =
                                          glm::normalize( ab ) * RiverFallSpeed( fall.FlowSpeed, drop, gravity ),
                                                 .Immersion = 0.5f * kRiverFallSheetThicknessCm - std::abs( off ) };
            }
        return best;
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
        RiverFootprint box{ .Min = glm::vec2( 1.0f ), .Max = glm::vec2( 0.0f ) };
        if ( points.size() < 2 || !( width > 0.0f ) )
            return box;
        box.Min = glm::vec2( std::numeric_limits<float>::max() );
        box.Max = glm::vec2( std::numeric_limits<float>::lowest() );
        for ( const Sample& sample : SampleSpline( points ) )
        {
            box.Min = glm::min( box.Min, sample.Position );
            box.Max = glm::max( box.Max, sample.Position );
        }
        box.Min -= glm::vec2( 0.5f * width );
        box.Max += glm::vec2( 0.5f * width );
        return box;
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
                                  const float flowSpeed, const float flowPeriod, const float cmPerTexel ) noexcept
    {
        RiverFlowField field;
        field.FlowPeriod         = flowPeriod > 0.0f ? flowPeriod : 1.0f;
        const RiverFootprint box = ComputeRiverFootprint( points, width );
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
        // touches (cost ~ the ribbon's area, not texels x samples).
        const std::vector<Sample> samples   = SampleSpline( points );
        const float               halfWidth = 0.5f * width;
        const glm::vec2           step      = extent / glm::vec2( field.Size );
        std::vector<float>        nearest( field.Texels.size(), std::numeric_limits<float>::max() );
        // The stretch each texel took its values from: a texel nearest a steep stretch lies under a fall's sheet.
        std::vector<uint32_t> owner( field.Texels.size(), std::numeric_limits<uint32_t>::max() );
        double                sumY = 0.0;
        for ( const Sample& sample : samples )
            sumY += sample.Y;
        field.SurfaceY = static_cast<float>( sumY / static_cast<double>( samples.size() ) );

        for ( size_t s = 0; s + 1 < samples.size(); ++s )
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
        // THE FALLS: each steep run of stretches is a waterfall's sheet (RiverFall), and the texels it owns are no
        // level surface - no bank mask, no level - so neither the quadtree's river tiles nor a CPU query lay a
        // flat surface across the drop: the sheet is drawn there (WaterRenderer's fall rows) and answered there
        // (RiverFallAt).
        const std::vector<std::pair<size_t, size_t>> runs = SteepRuns( samples );
        for ( const std::pair<size_t, size_t>& run : runs )
            field.Falls.push_back( MakeFall( samples, run, width, flowSpeed ) );
        if ( !runs.empty() )
            for ( size_t i = 0; i < field.Texels.size(); ++i )
                for ( const std::pair<size_t, size_t>& run : runs )
                    if ( owner[i] >= run.first && owner[i] <= run.second )
                    {
                        field.Texels[i]          = glm::vec4( 0.0f );
                        field.SurfaceHeightCm[i] = 0.0f;
                        break;
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
             BakeRiverFlow( world, input.Width, input.FlowSpeed, input.FlowPeriod, kRiverFlowCmPerTexel ) );
    }
} // namespace Desert::Water
