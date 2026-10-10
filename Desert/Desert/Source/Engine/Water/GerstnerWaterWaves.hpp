#pragma once

// The CPU side of the water's waves (WATER-W1): UE's seeded Gerstner generator and the wave queries, every one of
// them evaluated through Shaders/Common/GerstnerWave.glslh — the text the water shader displaces its surface with.
//
// Ported from UE 5.8 Engine/Plugins/Experimental/Water/Source/Runtime: Public/GerstnerWaterWaves.h:19-44
// (FGerstnerWave), :100-143 (UGerstnerWaterWaveGeneratorSimple and its defaults),
// Private/GerstnerWaterWaves.cpp:115-139 (the generator), :100-104 (MaxWaveHeight),
// Private/GerstnerWaveEvaluation.cpp:73-153 (the summed queries). Adapted: Y-up — UE's (X, Y) plane is our (X, Z),
// and a plane point is (world X, world Z) relative to the water body's WaveOrigin, in centimetres; UE's derived
// fields (WaveVector, WaveSpeed, WKA, Q) are not stored but computed by the shared header from the four authored
// ones, so CPU and GPU cannot hold different copies of them.

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Water
{
    /// One wave (UE FGerstnerWave's authored fields). Centimetres; Direction is a unit vector in the plane.
    struct GerstnerWave
    {
        glm::vec2 Direction{ 1.0f, 0.0f };
        float     WaveLength = 25.0f;
        float     Amplitude  = 10.0f;
        /// 0 = a cosine swell; 1 = the crest just closes (the sideways reach equals 1 / k).
        float Steepness = 0.0f;

        [[nodiscard]] bool operator==( const GerstnerWave& ) const = default;
    };

    /// UE's UGerstnerWaterWaveGeneratorSimple, with UE's defaults: a seed and a handful of ranges stand for a
    /// whole wave set, which is what a `.dwaves` asset stores.
    struct GerstnerWaveGenerator
    {
        int32_t NumWaves          = 16;
        int32_t Seed              = 0;
        float   Randomness        = 0.0f;
        float   MinWavelength     = 521.0f;
        float   MaxWavelength     = 6000.0f;
        float   WavelengthFalloff = 2.0f;
        float   MinAmplitude      = 4.0f;
        float   MaxAmplitude      = 80.0f;
        float   AmplitudeFalloff  = 2.0f;
        /// The dominant wind direction, degrees from +X toward +Z.
        float WindAngleDeg              = 0.0f;
        float DirectionAngularSpreadDeg = 1325.0f;
        float SmallWaveSteepness        = 0.4f;
        float LargeWaveSteepness        = 0.2f;
        float SteepnessFalloff          = 1.0f;

        [[nodiscard]] bool operator==( const GerstnerWaveGenerator& ) const = default;
    };

    /// The waves a generator stands for, in UE's order and from UE's random stream (the same seed gives UE's
    /// waves). NumWaves below 1 gives none.
    [[nodiscard]] std::vector<GerstnerWave> GenerateGerstnerWaves( const GerstnerWaveGenerator& generator );

    /// The sum of the amplitudes: no point of the surface rises higher or sinks lower (UE MaxWaveHeight).
    [[nodiscard]] float MaxWaveHeight( std::span<const GerstnerWave> waves );

    /// The summed displacement of the plane point at `time` seconds, world axes (X, Y = up, Z): what the GPU
    /// moves the surface vertex at `plane` by.
    [[nodiscard]] glm::vec3 WaveDisplacement( std::span<const GerstnerWave> waves, glm::vec2 plane, float time );

    struct WaveHeightSample
    {
        /// Above the still water level, cm.
        float Height = 0.0f;
        /// Unit, world axes.
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
    };

    /// The surface height and normal straight above the FIXED plane point (UE GetWaveHeightAtPosition): what a
    /// floating object or a swimmer at that point meets.
    [[nodiscard]] WaveHeightSample WaveHeightAt( std::span<const GerstnerWave> waves, glm::vec2 plane,
                                                 float time );

    /// The sum of A cos(phase) at the plane point, without the sideways correction (UE
    /// GetSimpleWaveHeightAtPosition).
    [[nodiscard]] float WaveSimpleHeightAt( std::span<const GerstnerWave> waves, glm::vec2 plane, float time );

    /// The share of the waves that survives over water `depth` cm deep (UE GetWaveAttenuationFactor).
    [[nodiscard]] float WaveDepthAttenuation( float depth, float targetWaveMaskDepth );
} // namespace Desert::Water
