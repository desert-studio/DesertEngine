#include <Engine/Water/GerstnerWaterWaves.hpp>

#include <Common/Core/GlslAsCpp.hpp>
#include <Common/Core/Math/RandomStream.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Desert::Water
{
    namespace
    {
        // Shaders/Common/GerstnerWave.glslh COMPILED AS C++ — the text the water shader displaces and shades the
        // surface with, so the CPU's waves and the drawn waves are one function. The shader root is on the include
        // path of every project that compiles this file.
        using glm::abs;
        using glm::clamp;
        using glm::cos;
        using glm::dot;
        using glm::exp;
        using glm::length;
        using glm::max;
        using glm::mix;
        using glm::sin;
        using glm::sqrt;
        using glm::trunc;
        using glm::vec2;
        using glm::vec3;
        using glm::vec4;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/GerstnerWave.glslh>
             DESERT_GLSL_AS_CPP_END

             // UE FMath::Lerp: A + Alpha * (B - A).
             float
             Lerp( float a, float b, float alpha )
        {
            return a + alpha * ( b - a );
        }
    } // namespace

    std::vector<GerstnerWave> GenerateGerstnerWaves( const GerstnerWaveGenerator& g )
    {
        std::vector<GerstnerWave> waves;
        if ( g.NumWaves < 1 )
            return waves;
        waves.reserve( static_cast<size_t>( g.NumWaves ) );

        Common::Math::RandomStream stream( g.Seed );
        const auto                 count = static_cast<float>( g.NumWaves );
        // UE builds the direction in FVector, i.e. in double (GerstnerWaterWaves.cpp:126-130).
        const double wind = static_cast<double>( g.WindAngleDeg ) * std::numbers::pi / 180.0;
        for ( int32_t i = 0; i < g.NumWaves; ++i )
        {
            const auto  fi    = static_cast<float>( i );
            const float alpha = std::clamp(
                 1.0f - ( fi / count ) +
                      stream.FRandRange( g.Randomness * ( -1.0f / count ), g.Randomness * ( 1.0f / count ) ),
                 0.0f, 1.0f );

            double x = std::cos( wind );
            double z = std::sin( wind );
            if ( i > 0 )
            {
                // FVector::RotateAngleAxis about the up axis.
                const double spread = static_cast<double>( stream.FRandRange( -g.DirectionAngularSpreadDeg,
                                                                              g.DirectionAngularSpreadDeg ) ) *
                                      std::numbers::pi / 180.0;
                const double rx = x * std::cos( spread ) - z * std::sin( spread );
                const double rz = x * std::sin( spread ) + z * std::cos( spread );
                x               = rx;
                z               = rz;
            }

            GerstnerWave& wave = waves.emplace_back();
            wave.Direction     = glm::vec2( static_cast<float>( x ), static_cast<float>( z ) );
            wave.WaveLength    = Lerp( g.MinWavelength, g.MaxWavelength, std::pow( alpha, g.WavelengthFalloff ) );
            wave.Amplitude     = std::max(
                 Lerp( g.MinAmplitude, g.MaxAmplitude, std::pow( alpha, g.AmplitudeFalloff ) ), 0.0001f );
            wave.Steepness =
                 Lerp( g.LargeWaveSteepness, g.SmallWaveSteepness, std::pow( fi / count, g.SteepnessFalloff ) );
        }
        return waves;
    }

    float MaxWaveHeight( std::span<const GerstnerWave> waves )
    {
        float height = 0.0f;
        for ( const GerstnerWave& w : waves )
            height += w.Amplitude;
        return height;
    }

    glm::vec3 WaveDisplacement( std::span<const GerstnerWave> waves, glm::vec2 plane, float time )
    {
        glm::vec3 sum( 0.0f );
        for ( const GerstnerWave& w : waves )
            sum += GerstnerWaveOffset( plane, w.Direction, w.WaveLength, w.Amplitude, w.Steepness, time );
        return sum;
    }

    WaveHeightSample WaveHeightAt( std::span<const GerstnerWave> waves, glm::vec2 plane, float time )
    {
        float     height = 0.0f;
        glm::vec3 terms( 0.0f );
        for ( const GerstnerWave& w : waves )
        {
            const glm::vec4 sample =
                 GerstnerWaveHeightAt( plane, w.Direction, w.WaveLength, w.Amplitude, w.Steepness, time );
            terms += glm::vec3( sample.x, sample.y, sample.z );
            height += sample.w;
        }
        return { height, GerstnerFinalizeNormal( terms ) };
    }

    float WaveSimpleHeightAt( std::span<const GerstnerWave> waves, glm::vec2 plane, float time )
    {
        float height = 0.0f;
        for ( const GerstnerWave& w : waves )
            height += GerstnerWaveSimpleHeight( plane, w.Direction, w.WaveLength, w.Amplitude, time );
        return height;
    }

    float WaveDepthAttenuation( float depth, float targetWaveMaskDepth )
    {
        return GerstnerDepthAttenuation( depth, targetWaveMaskDepth );
    }
} // namespace Desert::Water
