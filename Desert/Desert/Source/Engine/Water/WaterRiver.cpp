#include <Engine/Water/WaterRiver.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

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
    } // namespace

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

    glm::vec4 SampleRiverFlow( const RiverFlowField& field, const glm::vec2 xz ) noexcept
    {
        if ( field.Size.x == 0u || field.Size.y == 0u || field.Texels.empty() )
            return glm::vec4( 0.0f );
        const glm::vec2 uv = ( xz - field.Min ) / field.Extent;
        if ( !( uv.x >= 0.0f && uv.x < 1.0f && uv.y >= 0.0f && uv.y < 1.0f ) )
            return glm::vec4( 0.0f );
        const auto x = std::min( static_cast<uint32_t>( uv.x * static_cast<float>( field.Size.x ) ), field.Size.x - 1u );
        const auto z = std::min( static_cast<uint32_t>( uv.y * static_cast<float>( field.Size.y ) ), field.Size.y - 1u );
        return field.Texels[static_cast<size_t>( z ) * field.Size.x + x];
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

        // The nearest polyline segment per texel, rasterised segment by segment over the texels its swept box
        // touches (cost ~ the ribbon's area, not texels x samples).
        const std::vector<Sample> samples   = SampleSpline( points );
        const float               halfWidth = 0.5f * width;
        const glm::vec2           step      = extent / glm::vec2( field.Size );
        std::vector<float>        nearest( field.Texels.size(), std::numeric_limits<float>::max() );
        double                    sumY = 0.0;
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
                    glm::vec2   direction = glm::mix( a.Direction, b.Direction, u );
                    const float len       = glm::length( direction );
                    direction             = len > 1e-6f ? direction / len : glm::vec2( 0.0f );
                    // A parabolic cross-section profile: full speed on the centre line, still water at the bank.
                    const float across = d / halfWidth;
                    const float ease   = 1.0f - across * across;
                    field.Texels[i]    = glm::vec4( direction * ( flowSpeed * ease ), 1.0f,
                                                    glm::mix( a.Distance, b.Distance, u ) );
                }
        }
        return field;
    }

    uint64_t RiverBakeKey( const RiverBakeInput& input ) noexcept
    {
        uint64_t   key = 1469598103934665603ull;
        const auto mix = [&key]( const float v ) { key = ( key ^ std::bit_cast<uint32_t>( v ) ) * 1099511628211ull; };
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
