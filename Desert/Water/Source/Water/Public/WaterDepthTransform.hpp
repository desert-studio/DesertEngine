#pragma once

// What the depth does to a wave on the CPU: the C++ of Shaders/Water/WaterDepthTransform.glslh, compiled from that
// very text, so what floats and what is drawn shoal, break and run up the beach by one function.

#include <glm/glm.hpp>

namespace Desert::Water
{
    inline constexpr float kBreakerIndex = 0.78f; // H / h where a wave breaks (Dean & Dalrymple)

    struct WaterDepthTransform
    {
        float Amplitude; // factor of the band's deep-water amplitude: shoaling under the breaker cap, 0 on land
        float Breaking;  // 0..1, how much of the band's height the cap removed
    };

    /// omega^2 = g k tanh(k h), solved by Eckart's explicit form (error under 5 %).
    [[nodiscard]] float ShoaledWaveNumber( float omega, float depthCm, float gravityCm ) noexcept;

    /// @p significantHeightCm the band's Hs in deep water, @p omega its peak frequency.
    [[nodiscard]] WaterDepthTransform TransformBand( float significantHeightCm, float omega, float depthCm,
                                                     float gravityCm ) noexcept;

    /// THE SHORE WAVE: bores of the swell that reached the surf zone, phase = omega t + the integral of k over the
    /// shore distance, height kBreakerIndex * depth, plus the swash past the still line.
    struct ShoreWaveParams
    {
        float Omega             = 0.0f;
        float SignificantHeight = 0.0f; // of the swell band, cm
        float BeachSlope        = 0.0f; // the field's (WaterShoreField::BeachSlope)
    };
    struct ShoreWaveSample
    {
        glm::vec3 Displacement{ 0.0f };
        glm::vec2 SlopeXZ{ 0.0f };
        float     Front   = 0.0f; // 0..1 the bore's face: the foam source
        float     RunUpCm = 0.0f; // level of the swash tongue over the still level
    };
    [[nodiscard]] ShoreWaveSample EvaluateShoreWave( const ShoreWaveParams& params, float shoreDistanceCm,
                                                     glm::vec2 shoreDirection, float depthCm, glm::vec2 worldXZ,
                                                     float timeSeconds, float gravityCm ) noexcept;

    /// The share [0, 1] of the swell the shore wave carries there; the swell's own bands keep the rest.
    [[nodiscard]] float ShoreWaveWeight( float significantHeightCm, float depthCm,
                                         float shoreDistanceCm ) noexcept;
} // namespace Desert::Water
