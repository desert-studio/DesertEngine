#include <Water/Public/WaterDepthTransform.hpp>

#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Water
{
    namespace
    {
        using glm::clamp;
        using glm::cos;
        using glm::dot;
        using glm::exp;
        using glm::floor;
        using glm::max;
        using glm::min;
        using glm::sin;
        using glm::smoothstep;
        using glm::sqrt;
        using glm::vec2;

        // clang-format off
        DESERT_GLSL_AS_CPP_BEGIN
#include <Water/WaterDepthTransform.glslh>
        DESERT_GLSL_AS_CPP_END
        // clang-format on
    } // namespace

    float ShoaledWaveNumber( const float omega, const float depthCm, const float gravityCm ) noexcept
    {
        return WaterShoaledWaveNumber( omega, depthCm, gravityCm );
    }

    WaterDepthTransform TransformBand( const float significantHeightCm, const float omega, const float depthCm,
                                       const float gravityCm ) noexcept
    {
        const glm::vec2 band = WaterTransformBand( significantHeightCm, omega, depthCm, gravityCm );
        return { band.x, band.y };
    }

    ShoreWaveSample EvaluateShoreWave( const ShoreWaveParams& params, const float shoreDistanceCm,
                                       const glm::vec2 shoreDirection, const float depthCm,
                                       const glm::vec2 worldXZ, const float timeSeconds,
                                       const float gravityCm ) noexcept
    {
        const WaterShoreWave wave =
             WaterEvaluateShoreWave( params.Omega, params.SignificantHeight, params.BeachSlope, shoreDistanceCm,
                                     shoreDirection, depthCm, worldXZ, timeSeconds, gravityCm );
        ShoreWaveSample sample;
        sample.Displacement = glm::vec3( 0.0f, wave.Height, 0.0f );
        sample.SlopeXZ      = wave.Slope;
        sample.Front        = wave.Front;
        sample.RunUpCm      = wave.RunUp;
        return sample;
    }

    float ShoreWaveWeight( const float significantHeightCm, const float depthCm,
                           const float shoreDistanceCm ) noexcept
    {
        return WaterShoreWeight( significantHeightCm, depthCm, shoreDistanceCm );
    }
} // namespace Desert::Water
