#include <Water/Public/WaterBodyQuery.hpp>
#include <Water/Public/GerstnerWaterWaves.hpp>
#include <Water/Public/WaterDepthTransform.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace Desert::Water
{
    void SetWaveLane( WaterBodyState& body, const WaterWaveLane& lane, const WaterFFTSurface& surface )
    {
        body.Waves        = lane.Waves;
        body.WaveOrigin   = lane.Origin;
        body.WavesPending = lane.State == WaterWaveState::Pending;
        body.FFT          = lane.State == WaterWaveState::FFT;
        body.FFTSurface   = body.FFT ? surface.Mirror.Latest( lane.Spectrum.get() ) : nullptr;
    }

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
                                             RiverFallAt( *body.Flow, point ).has_value() );
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
            bool      Ready       = true;
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

        // The drawn surface's sum at a still particle (WaterSurfaceField.glslh WaterSurfaceAt): the body's one
        // source - the FFT cascades the GPU read back, or the Gerstner set - each band under the depth's transform.
        struct Bands
        {
            std::array<float, kFFTOceanCascades> Weight{}; // [0] alone for a Gerstner set
            ShoreWaveParams                      Swell;
            WaterShoreSample                     Shore;
            float                                Depth = 0.0f;
        };

        bool Readable( const std::shared_ptr<const FFTSurfaceSnapshot>& surface )
        {
            return surface != nullptr && surface->Spectrum != nullptr &&
                   surface->Current.Texels.size() == kFFTReadbackTexels;
        }

        std::span<const GerstnerWave> GerstnerOf( const WaterBodyState& body )
        {
            const std::span<const GerstnerWave> set( *body.Waves );
            return set.first( std::min( set.size(), kMaxDrawnWaves ) );
        }

        Bands BandsAt( const WaterBodyState& body, const glm::vec2 still, const float planeY,
                       const WaterShoreField* shore, const float beachSlope )
        {
            Bands bands;
            bands.Shore.GroundCm        = planeY - kOpenSeaDepthCm;
            bands.Shore.ShoreDistanceCm = kShoreFarCm;
            if ( shore != nullptr )
                bands.Shore = SampleWaterShoreField( *shore, still );
            bands.Depth = planeY - bands.Shore.GroundCm;
            bands.Swell = body.FFT ? FFTOceanSwell( *body.FFTSurface->Spectrum, beachSlope )
                                   : GerstnerSwell( GerstnerOf( body ), beachSlope );
            const float nearShore =
                 beachSlope > 0.0f
                      ? ShoreWaveWeight( bands.Swell.SignificantHeight, bands.Depth, bands.Shore.ShoreDistanceCm )
                      : 0.0f;
            if ( !body.FFT )
            {
                bands.Weight[0] = TransformBand( bands.Swell.SignificantHeight, bands.Swell.Omega, bands.Depth,
                                                 kWaveGravityCm )
                                       .Amplitude *
                                  ( 1.0f - nearShore );
                return bands;
            }
            for ( uint32_t c = 0; c < kFFTOceanCascades; ++c )
            {
                const FFTOceanCascade& cascade = body.FFTSurface->Spectrum->Cascades[c];
                bands.Weight[c] =
                     TransformBand( cascade.SignificantHeight, cascade.PeakOmega, bands.Depth, kWaveGravityCm )
                          .Amplitude *
                     ( c < kShoreSwellBands ? 1.0f - nearShore : 1.0f );
            }
            return bands;
        }

        glm::vec3 DisplacementAt( const WaterBodyState& body, const glm::vec2 still, const float time,
                                  const Bands& bands )
        {
            const glm::vec2 plane =
                 still - body.WaveOrigin.value_or( glm::vec2( body.Location.x, body.Location.z ) );
            return body.FFT
                        ? FFTDisplacementAt( *body.FFTSurface, plane, static_cast<double>( time ), bands.Weight )
                        : WaveDisplacement( GerstnerOf( body ), plane, time ) * bands.Weight[0];
        }

        // Half the span the surface's tangents are taken over, cm: the finest detail the source holds.
        float TangentReachCm( const WaterBodyState& body )
        {
            constexpr float kGerstnerReachCm = 5.0f;
            return body.FFT ? 0.5f * body.FFTSurface->Spectrum->Params.CascadeSizes[kFFTOceanCascades - 1] /
                                   static_cast<float>( kFFTReadbackSize )
                            : kGerstnerReachCm;
        }

        // The steps of the fixed-point search for the still particle: each shrinks the error by the choppiness.
        constexpr int kStillParticleSteps = 4;

        Waves WavesOver( const WaterBodyState& body, const glm::vec2 worldXZ, const float planeY, const float time,
                         const WaterShoreField* shore, const float beachSlope )
        {
            Waves waves;
            if ( body.WavesPending ||
                 ( body.FFT ? !Readable( body.FFTSurface ) : body.Waves == nullptr || body.Waves->empty() ) )
            {
                // A flat body is its plane; a set not read yet, or an FFT sea not read back yet, is not known.
                waves.Ready = !body.WavesPending && !body.FFT;
                return waves;
            }
            // The particle of still water the waves have moved over the point: its depth, not the point's, is
            // what the drawn surface bends its bands by.
            glm::vec2 still = worldXZ;
            Bands     bands = BandsAt( body, still, planeY, shore, beachSlope );
            glm::vec3 moved = DisplacementAt( body, still, time, bands );
            for ( int i = 0; i < kStillParticleSteps; ++i )
            {
                still = worldXZ - glm::vec2( moved.x, moved.z );
                bands = BandsAt( body, still, planeY, shore, beachSlope );
                moved = DisplacementAt( body, still, time, bands );
            }
            waves.Height      = moved.y;
            waves.Attenuation = bands.Weight[0];
            waves.MaxHeight   = bands.Weight[0] * ( body.FFT ? body.FFTSurface->Spectrum->MaxWaveHeight
                                                             : MaxWaveHeight( GerstnerOf( body ) ) );
            const float e       = TangentReachCm( body );
            const auto  tangent = [&]( const glm::vec2 d )
            {
                return glm::vec3( 2.0f * d.x, 0.0f, 2.0f * d.y ) + DisplacementAt( body, still + d, time, bands ) -
                       DisplacementAt( body, still - d, time, bands );
            };
            const glm::vec3 normal = glm::cross( tangent( { 0.0f, e } ), tangent( { e, 0.0f } ) );
            // A fold (the Jacobian not positive) has no height-field normal: the plane's stands.
            if ( normal.y > 0.0f )
                waves.Normal = glm::normalize( normal );
            AddShoreWave( waves, bands.Swell, bands.Shore, bands.Depth, still, time, kWaveGravityCm );
            return waves;
        }
    } // namespace

    WaterQueryResult QueryWaterBody( const WaterBodyState& body, const glm::vec3& point, float time,
                                     const WaterShoreField* shore )
    {
        WaterQueryResult result;

        // A river's fall (WaterRiver.hpp RiverFall): off the banks the point is in the falling sheet, if anywhere -
        // the sheet is the surface (its mid-plane and normal), the water column is the sheet's thickness, and the
        // current is the falling water's (the baked flight's speed, RiverFallSample::Speed), so a floating body
        // caught by the fall is dragged down it (BuoyancySimulation's drag on the relative velocity).
        if ( body.Type == WaterBodyType::River && body.Flow != nullptr &&
             !RiverInsideBank( *body.Flow, glm::vec2( point.x, point.z ) ) )
        {
            if ( const std::optional<RiverFallHit> fall = RiverFallAt( *body.Flow, point ) )
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

        Waves waves = WavesOver( body, glm::vec2( point.x, point.z ), result.PlaneLocation.y, time, shore,
                                 shore != nullptr && body.Type != WaterBodyType::River ? shore->BeachSlope : 0.0f );
        // A river has no waves of its own: near its mouth it carries its receiver's (the lane SetWaveLane gave it),
        // fading to none Mouth.Radius upstream (RiverMouthWeight, the GPU's WaterMouthWeight).
        if ( body.Type == WaterBodyType::River )
        {
            const float weight =
                 body.Flow != nullptr ? RiverMouthWeight( *body.Flow, glm::vec2( point.x, point.z ) ) : 0.0f;
            waves.Height *= weight;
            waves.MaxHeight *= weight;
            waves.Normal =
                 glm::normalize( glm::vec3( waves.Normal.x * weight, waves.Normal.y, waves.Normal.z * weight ) );
        }
        result.SurfaceReady    = waves.Ready;
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

        // The boil under every fall that lands in this body pushes its water outward (PlungeVelocityAt, the foam
        // field's WaterPlungeAt): what carries a floating body away from the foot of a fall, river or ocean.
        for ( const FallPlunge& plunge : body.Plunges )
        {
            const glm::vec2 push = PlungeVelocityAt( plunge, glm::vec2( point.x, point.z ) );
            result.Velocity += glm::vec3( push.x, 0.0f, push.y );
        }
        return result;
    }

    std::optional<WaterQueryHit> QueryWater( std::span<const WaterBodyState> bodies,
                                             const WaterShoreField* shore, const glm::vec3& point, float time,
                                             const bool andUnready )
    {
        // The drawn surface's rule first: of the bodies covering the point only the highest WaterBodyPriority is
        // there (a river inside its banks is drawn over the ocean, the ocean under a river's cut leaf is clipped
        // to the banks' far side - WaterQuadTree::Classify, WaterRiver.glslh WaterBankClipped - so inside
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
            const bool answers = result.IsInWater() || ( andUnready && !result.SurfaceReady );
            if ( answers && ( !best || result.ImmersionDepth > best->Result.ImmersionDepth ) )
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
        m_Shore = std::move( shore );
    }

    std::optional<WaterQueryHit> WaterSubsystem::Query( const glm::vec3& point, const bool andUnready ) const
    {
        return QueryWater( m_Bodies, m_Shore.get(), point, Time(), andUnready );
    }
} // namespace Desert::Water
