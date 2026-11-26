#pragma once

#include <Engine/Core/PostProcessSettings.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// UE's Gaussian bloom (BM_SOG), the CPU half: the stage list a view's settings give and the 1D kernel each stage
// is blurred with. In a header the renderer and a test both compile, the numbers next to the UE lines they port.
namespace Desert::Graphic::BloomGaussian
{
    inline constexpr uint32_t kStageCount = 6;  // UE Bloom1..6: 1/2 .. 1/64 of the view (EBloomQuality::MAX)
    inline constexpr uint32_t kMaxSamples = 32; // UE MAX_FILTER_COMPILE_TIME_SAMPLES (r.Filter.LoopMode 0)

    // Bilinear-paired, normalised taps (UE Compute1DGaussianFilterKernel,
    // PostProcessWeightedSampleSum.cpp:149-187). x: offset in texels of the stage's own size, y: weight; the
    // weights sum to 1.
    struct Kernel
    {
        std::array<glm::vec4, kMaxSamples> OffsetWeight{};
        uint32_t                           SampleCount = 0;
    };

    // One stage of the sum: its tint (BloomNTint x 1/6 x Intensity x GaussianIntensity) and its Gaussian width in
    // percent of the stage's width (BloomNSize x BloomSizeScale).
    struct Stage
    {
        glm::vec3 Tint{ 0.0f };
        float     KernelSizePercent = 0.0f;
    };

    // UE GetBlurRadius (:189-195): stageWidth x percent x 0.01 x 0.5.
    [[nodiscard]] inline float BlurRadius( uint32_t stageWidth, float kernelSizePercent )
    {
        return static_cast<float>( stageWidth ) * kernelSizePercent * 0.01f * 0.5f;
    }

    // UE NormalDistributionUnscaled (:94-116): the legacy constant -16.7 keeps UE's look; a cross weight > 1 is
    // the r.Bloom.Cross power shape.
    [[nodiscard]] inline float NormalDistributionUnscaled( float x, float sigma, float crossCenterWeight )
    {
        const float dx                = std::abs( x );
        const float clampedOneMinusDX = std::max( 0.0f, 1.0f - dx );
        if ( crossCenterWeight > 1.0f )
            return std::pow( clampedOneMinusDX, crossCenterWeight );
        const float ratio    = dx / sigma;
        const float gaussian = std::exp( -16.7f * ratio * ratio );
        return gaussian + ( clampedOneMinusDX - gaussian ) * crossCenterWeight;
    }

    // UE Compute1DGaussianFilterKernel with r.Filter.SizeScale 1: the radius clamped to [1e-5, kMaxSamples - 1],
    // taps in pairs (the bilinear fetch between two texels), the last tap on its own texel centre.
    [[nodiscard]] inline Kernel Compute1DKernel( float radius, float crossCenterWeight = 0.0f )
    {
        constexpr float kDelta  = 0.00001f; // UE DELTA
        const float     clamped = std::clamp( radius, kDelta, static_cast<float>( kMaxSamples - 1 ) );
        const int       integerR =
             std::min( static_cast<int>( std::ceil( clamped ) ), static_cast<int>( kMaxSamples ) - 1 );

        Kernel kernel;
        float  weightSum = 0.0f;
        for ( int index = -integerR; index <= integerR && kernel.SampleCount < kMaxSamples; index += 2 )
        {
            const float weight0 =
                 NormalDistributionUnscaled( static_cast<float>( index ), clamped, crossCenterWeight );
            float weight1 = 0.0f;
            if ( index != integerR )
                weight1 =
                     NormalDistributionUnscaled( static_cast<float>( index + 1 ), clamped, crossCenterWeight );
            const float total                       = weight0 + weight1;
            kernel.OffsetWeight[kernel.SampleCount] = glm::vec4(
                 static_cast<float>( index ) + ( total > 0.0f ? weight1 / total : 0.0f ), total, 0.0f, 0.0f );
            weightSum += total;
            ++kernel.SampleCount;
        }
        const float inverse = weightSum > 0.0f ? 1.0f / weightSum : 0.0f;
        for ( uint32_t i = 0; i < kernel.SampleCount; ++i )
            kernel.OffsetWeight[i].y *= inverse;
        return kernel;
    }

    // The stages in UE's order (PostProcessBloomSetup.cpp:226-273): COARSEST FIRST, Bloom6 (1/64) .. Bloom1 (1/2).
    // Index i reads the downsample chain's stage kStageCount - 1 - i. A quality q keeps the first q (UE drops
    // Bloom1 first). A stage whose Size is ~0 has a zero tint: UE skips it, its V pass would add nothing but the
    // smaller.
    [[nodiscard]] inline std::array<Stage, kStageCount> Stages( const Core::PostProcessSettings& s )
    {
        const float tintScale =
             ( 1.0f / static_cast<float>( kStageCount ) ) * s.BloomIntensity * s.BloomGaussianIntensity;
        const glm::vec3                tints[kStageCount] = { s.Bloom6Tint, s.Bloom5Tint, s.Bloom4Tint,
                                                              s.Bloom3Tint, s.Bloom2Tint, s.Bloom1Tint };
        const float                    sizes[kStageCount] = { s.Bloom6Size, s.Bloom5Size, s.Bloom4Size,
                                                              s.Bloom3Size, s.Bloom2Size, s.Bloom1Size };
        std::array<Stage, kStageCount> stages{};
        for ( uint32_t i = 0; i < kStageCount; ++i )
        {
            stages[i].KernelSizePercent = sizes[i] * s.BloomSizeScale;
            stages[i].Tint              = sizes[i] > 1e-8f ? tints[i] * tintScale : glm::vec3( 0.0f );
        }
        return stages;
    }

    // UE PostProcessing.cpp:862-864: bloom runs when its intensity and the method's own intensity are above 0.
    [[nodiscard]] inline bool GaussianOn( const Core::PostProcessSettings& s )
    {
        return s.BloomIntensity > 0.0f && s.BloomGaussianIntensity > 0.0f;
    }
    [[nodiscard]] inline bool ConvolutionOn( const Core::PostProcessSettings& s )
    {
        return s.BloomIntensity > 0.0f && s.BloomConvolutionIntensity > 0.0f;
    }
    // The view's bloom runs at all: the gate of its method.
    [[nodiscard]] inline bool BloomOn( const Core::PostProcessSettings& s )
    {
        return s.BloomMethod == Core::BloomMethod::Convolution ? ConvolutionOn( s ) : GaussianOn( s );
    }
} // namespace Desert::Graphic::BloomGaussian
