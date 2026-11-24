#include <Water/Public/WaterBodyQuery.hpp>
#include <Water/Public/GerstnerWaterWaves.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <Engine/World/Landscape/LandscapeData.hpp>

namespace Desert::Water
{
    bool WaterBodyCovers( const WaterBodyState& body, const glm::vec3& point )
    {
        const glm::vec2 half = body.Extents * 0.5f;
        if ( std::abs( point.x - body.Location.x ) > half.x || std::abs( point.z - body.Location.z ) > half.y )
            return false;
        // A river is its ribbon, not its box: the baked bank mask, clipped by the very test the drawn surface
        // discards by (RiverInsideBank = WaterOutsideBank's complement), so no water is answered where none is
        // drawn. A river without a bake covers nothing.
        // Its falls are their sheets (RiverFallAt): the water a point meets where the river drops off a lip.
        if ( body.Type == WaterBodyType::River )
            return body.Flow != nullptr && ( RiverInsideBank( *body.Flow, glm::vec2( point.x, point.z ) ) ||
                                             RiverFallAt( *body.Flow, point, body.Gravity ).has_value() );
        return true;
    }

    std::optional<float> GroundHeightAt( std::span<const World::Landscape::LandscapeRayTile> ground, float x,
                                         float z )
    {
        for ( const World::Landscape::LandscapeRayTile& tile : ground )
        {
            if ( tile.Heights == nullptr )
                continue;
            if ( const std::optional<float> height =
                      World::Landscape::SampleLandscapeHeight( *tile.Heights, tile.Frame, x, z ) )
                return height;
        }
        return std::nullopt;
    }

    WaterInfoGround BakeWaterInfoGround( std::span<const World::Landscape::LandscapeRayTile> ground,
                                         const glm::vec2 centre, const float extentCm, const uint32_t size )
    {
        WaterInfoGround info;
        info.Min      = centre - glm::vec2( 0.5f * extentCm );
        info.ExtentCm = extentCm;
        info.Size     = size;
        info.HeightCm.assign( static_cast<std::size_t>( size ) * size, kWaterInfoNoGroundCm );
        const float texel = extentCm / static_cast<float>( std::max( size, 1u ) );
        for ( uint32_t z = 0; z < size; ++z )
            for ( uint32_t x = 0; x < size; ++x )
            {
                const glm::vec2 at = info.Min + ( glm::vec2( x, z ) + 0.5f ) * texel;
                if ( const std::optional<float> height = GroundHeightAt( ground, at.x, at.y ) )
                    info.HeightCm[static_cast<std::size_t>( z ) * size + x] = *height;
            }
        return info;
    }

    WaterQueryResult QueryWaterBody( const WaterBodyState& body, const glm::vec3& point, float time,
                                     std::optional<float> groundHeight )
    {
        WaterQueryResult result;

        // A river's fall (WaterRiver.hpp RiverFall): off the banks the point is in the falling sheet, if anywhere -
        // the sheet is the surface (its mid-plane and normal), the water column is the sheet's thickness, and the
        // current is the falling water's (RiverFallSpeed: the river's flow gaining by free fall down the slope), so
        // a floating body caught by the fall is dragged down it (BuoyancySimulation's drag on the relative velocity).
        if ( body.Type == WaterBodyType::River && body.Flow != nullptr &&
             !RiverInsideBank( *body.Flow, glm::vec2( point.x, point.z ) ) )
        {
            if ( const std::optional<RiverFallHit> fall = RiverFallAt( *body.Flow, point, body.Gravity ) )
            {
                result.PlaneLocation   = fall->SheetPoint;
                result.SurfaceLocation = fall->SheetPoint;
                result.PlaneNormal     = fall->Normal;
                result.SurfaceNormal   = fall->Normal;
                result.PlaneDepth      = kRiverFallSheetThicknessCm;
                result.SurfaceDepth    = kRiverFallSheetThicknessCm;
                result.ImmersionDepth  = fall->Immersion;
                result.Velocity        = fall->Velocity;
                return result;
            }
        }

        // The plane: an ocean's surface is level, at the body's own height (UE :677-701). A river's follows its
        // spline (UE :702-741, the spline's height and up vector at the closest input key): the baked surface
        // height under the point, the normal from its slope across one bake texel; its current is the baked flow.
        result.PlaneLocation = glm::vec3( point.x, body.Location.y, point.z );
        if ( body.Type == WaterBodyType::River && body.Flow != nullptr )
        {
            const RiverFlowField& flow = *body.Flow;
            const glm::vec2       xz( point.x, point.z );
            if ( const std::optional<float> y = SampleRiverSurfaceY( flow, xz ) )
            {
                result.PlaneLocation.y = *y;
                const glm::vec2 texel  = flow.Extent / glm::vec2( glm::max( flow.Size, glm::uvec2( 1u ) ) );
                const auto      along  = [&]( const glm::vec2 d )
                {
                    const std::optional<float> a = SampleRiverSurfaceY( flow, xz + d );
                    const std::optional<float> b = SampleRiverSurfaceY( flow, xz - d );
                    return a && b ? ( *a - *b ) / ( 2.0f * glm::length( d ) ) : 0.0f;
                };
                const glm::vec3 normal( -along( glm::vec2( texel.x, 0.0f ) ), 1.0f,
                                        -along( glm::vec2( 0.0f, texel.y ) ) );
                result.PlaneNormal = glm::normalize( normal );
            }
            // The current the surface advects its flow map by (WaterFlowAt), the same bilinear texels.
            const glm::vec3 f = SampleRiverFlowBilinear( flow, xz );
            result.Velocity   = glm::vec3( f.x, 0.0f, f.y );
        }
        result.SurfaceLocation = result.PlaneLocation;
        result.SurfaceNormal   = result.PlaneNormal;

        // The depth: down to the landscape; an ocean over no landscape, or under it, reports the fallback depth
        // and has its waves cancelled where the landscape stands above the plane (UE :743-794).
        float waveAttenuation = 1.0f;
        float planeDepth      = 0.0f;
        if ( groundHeight )
        {
            planeDepth = result.PlaneLocation.y - *groundHeight;
            if ( planeDepth < 0.0f )
                waveAttenuation = 0.0f;
        }
        if ( !groundHeight || planeDepth < 0.0f )
            planeDepth = kOceanFallbackDepthCm;
        result.PlaneDepth   = glm::max( planeDepth, 0.0f );
        result.SurfaceDepth = result.PlaneDepth;

        // The waves: attenuated by depth, evaluated at the fixed plane point relative to the WaveOrigin
        // (UE :801-831 and GetWaveInfoAtPosition :2012-2051).
        const std::optional<FFTSurfaceSample> fft =
             body.FFTSurface != nullptr
                  ? FFTSurfaceAt( *body.FFTSurface,
                                  glm::vec2( point.x - body.Location.x, point.z - body.Location.z ), time )
                  : std::nullopt;
        if ( fft )
        {
            // THE FFT OCEAN (WATER-RIVERS-FFT pass 7): the GPU's cascades as the CPU mirror last read them,
            // extrapolated to `time`, damped by the same depth attenuation the vertex path applies.
            waveAttenuation *= WaveDepthAttenuation( result.SurfaceDepth, body.TargetWaveMaskDepth );
            result.WaveAttenuation = waveAttenuation;
            if ( waveAttenuation > 0.0f && body.FFTSurface->Spectrum != nullptr )
            {
                result.MaxWaveHeight = body.FFTSurface->Spectrum->MaxWaveHeight * waveAttenuation;
                const glm::vec3 normal = glm::mix( result.PlaneNormal, fft->Normal, waveAttenuation );
                if ( glm::dot( normal, normal ) > 0.0f )
                    result.SurfaceNormal = glm::normalize( normal );
                result.WaveHeight = fft->Height * waveAttenuation;
                result.SurfaceLocation.y += result.WaveHeight;
                result.SurfaceDepth += result.WaveHeight;
            }
        }
        else if ( body.Waves && !body.Waves->empty() )
        {
            const std::span<const GerstnerWave> waves( *body.Waves );
            waveAttenuation *= WaveDepthAttenuation( result.SurfaceDepth, body.TargetWaveMaskDepth );
            result.WaveAttenuation = waveAttenuation;
            if ( waveAttenuation > 0.0f )
            {
                result.MaxWaveHeight = MaxWaveHeight( waves ) * waveAttenuation;

                const glm::vec2        plane( point.x - body.Location.x, point.z - body.Location.z );
                const WaveHeightSample sample = WaveHeightAt( waves, plane, time );

                // UE lerps the normal toward the plane's by the same share and leaves it unnormalised; a
                // surface normal is a direction, so it is normalised here.
                const glm::vec3 normal = glm::mix( result.PlaneNormal, sample.Normal, waveAttenuation );
                if ( glm::dot( normal, normal ) > 0.0f )
                    result.SurfaceNormal = glm::normalize( normal );

                result.WaveHeight = sample.Height * waveAttenuation;
                result.SurfaceLocation.y += result.WaveHeight;
                result.SurfaceDepth += result.WaveHeight;
            }
        }

        // Immersion: how far under the waved surface the point is (UE :834-842).
        result.ImmersionDepth = result.SurfaceLocation.y - point.y;
        return result;
    }

    std::optional<WaterQueryHit> QueryWater( std::span<const WaterBodyState>                     bodies,
                                             std::span<const World::Landscape::LandscapeRayTile> ground,
                                             const glm::vec3& point, float time )
    {
        // The drawn surface's rule first: of the bodies covering the point only the highest WaterBodyPriority is
        // there (a river inside its banks is drawn over the ocean, the ocean under a river's cut leaf is clipped
        // to the banks' far side - WaterQuadTree::Classify, WaterMeshTiles.glslh WaterBankClipped - so inside
        // the banks the river answers, outside them the ocean). Of several of that priority, the one the point is
        // deepest in (UE BuoyancyComponentSimulation.h:199-231). A point above a river inside its banks is NOT in
        // the ocean below: the ocean is not drawn there.
        std::optional<WaterQueryHit> best;
        std::optional<int32_t>       topPriority;
        std::optional<float>         groundHeight;
        bool                         groundSampled = false;
        for ( std::size_t i = 0; i < bodies.size(); ++i )
        {
            if ( !WaterBodyCovers( bodies[i], point ) )
                continue;
            const int32_t priority = WaterBodyPriority( bodies[i].Type );
            if ( topPriority && priority < *topPriority )
                continue;
            if ( !topPriority || priority > *topPriority )
            {
                topPriority = priority;
                best.reset();
            }
            if ( !groundSampled )
            {
                groundHeight  = GroundHeightAt( ground, point.x, point.z );
                groundSampled = true;
            }
            const WaterQueryResult result = QueryWaterBody( bodies[i], point, time, groundHeight );
            if ( result.IsInWater() && ( !best || result.ImmersionDepth > best->Result.ImmersionDepth ) )
                best = WaterQueryHit{ i, result };
        }
        return best;
    }

    void WaterSubsystem::Advance( double stepSeconds )
    {
        m_Time += stepSeconds;
    }

    void WaterSubsystem::SetTime( double seconds )
    {
        m_Time = seconds;
    }

    void WaterSubsystem::SetWorld( std::span<const WaterBodyState>                     bodies,
                                   std::span<const World::Landscape::LandscapeRayTile> ground )
    {
        m_Bodies.assign( bodies.begin(), bodies.end() );
        for ( WaterBodyState& body : m_Bodies )
            body.Gravity = m_Gravity;
        m_Ground.assign( ground.begin(), ground.end() );
    }

    std::optional<WaterQueryHit> WaterSubsystem::Query( const glm::vec3& point ) const
    {
        return QueryWater( m_Bodies, m_Ground, point, Time() );
    }
} // namespace Desert::Water
