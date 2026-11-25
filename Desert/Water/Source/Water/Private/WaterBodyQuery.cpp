#include <Water/Public/WaterBodyQuery.hpp>
#include <Water/Public/GerstnerWaterWaves.hpp>
#include <Water/Public/WaterDepthTransform.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

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

    namespace
    {
        // The waves over one plane point, the depth's transform of each band taken (WaterSurfaceField.glslh
        // WaterSurfaceAt is the GPU's statement of the same sum).
        struct Waves
        {
            float     Height = 0.0f;
            glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
            float     Attenuation = 0.0f;
            float     MaxHeight   = 0.0f;
        };

        void AddShoreWave( Waves& waves, const ShoreWaveParams& params, const WaterShoreSample& at,
                           const float depth, const glm::vec2 worldXZ, const float time, const float gravity )
        {
            const ShoreWaveSample shore = EvaluateShoreWave( params, at.ShoreDistanceCm, at.ShoreDirection, depth,
                                                             worldXZ, time, gravity );
            waves.Height += shore.Displacement.y;
            waves.MaxHeight = std::max( waves.MaxHeight, shore.RunUpCm + kBreakerIndex * std::max( depth, 0.0f ) );
            // The slopes add: the normal of a sum of height fields.
            const glm::vec2 slope =
                 -glm::vec2( waves.Normal.x, waves.Normal.z ) / std::max( waves.Normal.y, 0.05f ) + shore.SlopeXZ;
            waves.Normal = glm::normalize( glm::vec3( -slope.x, 1.0f, -slope.y ) );
        }

        Waves WavesOver( const WaterBodyState& body, const glm::vec2 worldXZ, const float time, const float depth,
                         const WaterShoreSample& at, const float beachSlope )
        {
            Waves           waves;
            const glm::vec2 plane = worldXZ - glm::vec2( body.Location.x, body.Location.z );
            if ( body.FFTSurface != nullptr && body.FFTSurface->Spectrum != nullptr )
            {
                const FFTOceanSpectrum& spectrum = *body.FFTSurface->Spectrum;
                const ShoreWaveParams   swell    = FFTOceanSwell( spectrum, beachSlope );
                const float nearShore = beachSlope > 0.0f ? ShoreWaveWeight( swell.SignificantHeight, depth,
                                                                             at.ShoreDistanceCm )
                                                          : 0.0f;
                std::array<float, kFFTOceanCascades> bands{};
                for ( uint32_t c = 0; c < kFFTOceanCascades; ++c )
                {
                    const FFTOceanCascade& cascade = spectrum.Cascades[c];
                    bands[c] =
                         TransformBand( cascade.SignificantHeight, cascade.PeakOmega, depth, kWaveGravityCm )
                              .Amplitude *
                         ( c < kShoreSwellBands ? 1.0f - nearShore : 1.0f );
                }
                const std::optional<FFTSurfaceSample> fft =
                     FFTSurfaceAt( *body.FFTSurface, plane, static_cast<double>( time ), bands );
                if ( !fft )
                    return waves;
                waves.Height      = fft->Height;
                waves.Normal      = fft->Normal;
                waves.Attenuation = bands[0];
                waves.MaxHeight   = spectrum.MaxWaveHeight * bands[0];
                AddShoreWave( waves, swell, at, depth, worldXZ, time, kWaveGravityCm );
                return waves;
            }
            if ( body.Waves == nullptr || body.Waves->empty() )
                return waves;
            const std::span<const GerstnerWave> set( *body.Waves );
            const ShoreWaveParams               swell = GerstnerSwell( set, beachSlope );
            const float nearShore = beachSlope > 0.0f ? ShoreWaveWeight( swell.SignificantHeight, depth,
                                                                         at.ShoreDistanceCm )
                                                      : 0.0f;
            waves.Attenuation =
                 TransformBand( swell.SignificantHeight, swell.Omega, depth, kWaveGravityCm ).Amplitude *
                 ( 1.0f - nearShore );
            const WaveHeightSample sample = WaveHeightAt( set, plane, time );
            waves.Height                  = sample.Height * waves.Attenuation;
            waves.MaxHeight               = MaxWaveHeight( set ) * waves.Attenuation;
            const glm::vec3 normal = glm::mix( glm::vec3( 0.0f, 1.0f, 0.0f ), sample.Normal, waves.Attenuation );
            if ( glm::dot( normal, normal ) > 0.0f )
                waves.Normal = glm::normalize( normal );
            AddShoreWave( waves, swell, at, depth, worldXZ, time, kWaveGravityCm );
            return waves;
        }
    } // namespace

    WaterQueryResult QueryWaterBody( const WaterBodyState& body, const glm::vec3& point, float time,
                                     const WaterShoreField* shore )
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

        // The depth: down to the shore field's floor, the very texels the surface is drawn over. The ground above
        // the plane is a negative depth, not an unknown one. A world without its field yet is the open sea.
        WaterShoreSample at;
        at.GroundCm        = result.PlaneLocation.y - kOpenSeaDepthCm;
        at.ShoreDistanceCm = kShoreFarCm;
        if ( shore != nullptr )
            at = SampleWaterShoreField( *shore, glm::vec2( point.x, point.z ) );
        result.PlaneDepth   = result.PlaneLocation.y - at.GroundCm;
        result.SurfaceDepth = result.PlaneDepth;

        const Waves waves      = WavesOver( body, glm::vec2( point.x, point.z ), time, result.PlaneDepth, at,
                                            shore != nullptr && body.Type != WaterBodyType::River ? shore->BeachSlope : 0.0f );
        result.WaveAttenuation = waves.Attenuation;
        result.MaxWaveHeight   = waves.MaxHeight;
        result.WaveHeight      = waves.Height;
        result.SurfaceLocation.y += waves.Height;
        result.SurfaceDepth += waves.Height;
        // The waves stand on the plane: their slope and the plane's add.
        const glm::vec2 slope =
             glm::vec2( waves.Normal.x, waves.Normal.z ) / std::max( waves.Normal.y, 0.05f ) +
             glm::vec2( result.PlaneNormal.x, result.PlaneNormal.z ) / std::max( result.PlaneNormal.y, 0.05f );
        result.SurfaceNormal = glm::normalize( glm::vec3( slope.x, 1.0f, slope.y ) );

        // Immersion: how far under the waved surface the point is (UE :834-842).
        // No water stands where the surface is under the ground (the beach between two run-ups).
        result.ImmersionDepth =
             result.SurfaceDepth > 0.0f ? result.SurfaceLocation.y - point.y : -std::abs( result.SurfaceDepth );
        return result;
    }

    std::optional<WaterQueryHit> QueryWater( std::span<const WaterBodyState> bodies,
                                             const WaterShoreField* shore, const glm::vec3& point, float time )
    {
        // The drawn surface's rule first: of the bodies covering the point only the highest WaterBodyPriority is
        // there (a river inside its banks is drawn over the ocean, the ocean under a river's cut leaf is clipped
        // to the banks' far side - WaterQuadTree::Classify, WaterMeshTiles.glslh WaterBankClipped - so inside
        // the banks the river answers, outside them the ocean). Of several of that priority, the one the point is
        // deepest in (UE BuoyancyComponentSimulation.h:199-231). A point above a river inside its banks is NOT in
        // the ocean below: the ocean is not drawn there.
        std::optional<WaterQueryHit> best;
        std::optional<int32_t>       topPriority;
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
            const WaterQueryResult result = QueryWaterBody( bodies[i], point, time, shore );
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

    void WaterSubsystem::SetWorld( std::span<const WaterBodyState>        bodies,
                                   std::shared_ptr<const WaterShoreField> shore )
    {
        m_Bodies.assign( bodies.begin(), bodies.end() );
        for ( WaterBodyState& body : m_Bodies )
            body.Gravity = m_Gravity;
        m_Shore = std::move( shore );
    }

    std::optional<WaterQueryHit> WaterSubsystem::Query( const glm::vec3& point ) const
    {
        return QueryWater( m_Bodies, m_Shore.get(), point, Time() );
    }
} // namespace Desert::Water
