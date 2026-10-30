#pragma once

// LOCAL EXPOSURE — the CPU half of UE's Local Exposure, Bilateral method (UE 5 PostProcessLocalExposure.cpp,
// PostProcessHistogramCommon.ush:251-310, PostProcessTonemap.usf:411-423). Pure: what a view's blended settings
// resolve to, when the passes run, the dispatch geometry, and a mirror of the shader's exposure law
// (Shaders/Common/LocalExposure.glslh is the one source; LogLocalExposure below is asserted equal to it).
//
// Where this differs from UE on purpose (DesertEngine-scratch/LOCAL-EXPOSURE/CONTRACT.md §6):
//  1. one setup pass writes the bilateral grid AND the 1/32 log luminance (UE: grid pass + 4 downsamples + setup);
//  2. the grid's 32 slices are 1 EV each, centred on the exposed middle grey of the last exposure (UE slices by
//     the histogram window, ~1 EV at its defaults but stretched by ours to the sun's ceiling);
//  3. the threshold smoothstep has an explicit strength >= 1 branch (UE divides by 1 - strength = 0 at default);
//  4. the blur radius is not clamped to 31 texels, so the kernel is the same fraction of the view at any size.

#include <Engine/Core/PostProcessSettings.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Desert::Graphic
{
    // The resolved, shader-facing values of one view.
    struct LocalExposureParams
    {
        Core::LocalExposureMethod Method                     = Core::LocalExposureMethod::Bilateral;
        float HighlightContrast          = 1.0f;
        float ShadowContrast             = 1.0f;
        float DetailStrength             = 1.0f;
        float BlurredLumBlend            = 0.6f;
        float HighlightThreshold         = 0.0f;
        float ShadowThreshold            = 0.0f;
        float HighlightThresholdStrength = 1.0f;
        float ShadowThresholdStrength    = 1.0f;
        float MiddleGreyBiasEV           = 0.0f;
        float BlurKernelSizePercent      = 50.0f;
    };

    [[nodiscard]] inline LocalExposureParams ResolveLocalExposure( const Core::PostProcessSettings& post )
    {
        return LocalExposureParams{
             .Method                     = post.LocalExposureMethod,
             .HighlightContrast          = std::clamp( post.LocalExposureHighlightContrastScale, 0.0f, 1.0f ),
             .ShadowContrast             = std::clamp( post.LocalExposureShadowContrastScale, 0.0f, 1.0f ),
             .DetailStrength             = std::clamp( post.LocalExposureDetailStrength, 0.0f, 4.0f ),
             .BlurredLumBlend            = std::clamp( post.LocalExposureBlurredLuminanceBlend, 0.0f, 1.0f ),
             .HighlightThreshold         = std::clamp( post.LocalExposureHighlightThreshold, 0.0f, 4.0f ),
             .ShadowThreshold            = std::clamp( post.LocalExposureShadowThreshold, 0.0f, 4.0f ),
             .HighlightThresholdStrength = std::clamp( post.LocalExposureHighlightThresholdStrength, 0.0f, 1.0f ),
             .ShadowThresholdStrength    = std::clamp( post.LocalExposureShadowThresholdStrength, 0.0f, 1.0f ),
             .MiddleGreyBiasEV           = std::clamp( post.LocalExposureMiddleGreyBias, -15.0f, 15.0f ),
             .BlurKernelSizePercent =
                  std::clamp( post.LocalExposureBlurredLuminanceKernelSizePercent, 0.0f, 100.0f ) };
    }

    // UE's enable test (PostProcessing.cpp:799-805): contrast 1/1 and detail 1 give every pixel exactly 1 whatever
    // the thresholds and the blend (with contrast 1 the threshold offset cancels, PostProcessHistogramCommon.ush:
    // 287-307: LogLocalLum = MG + (Base - MG) + (Lum - Base) = Lum).
    // Fusion ignores the detail strength: contrast 1/1 makes the three exposures one and the pyramid rebuilds it.
    [[nodiscard]] inline bool IsLocalExposureIdentity( const LocalExposureParams& p )
    {
        if ( p.Method == Core::LocalExposureMethod::Fusion )
            return p.HighlightContrast == 1.0f && p.ShadowContrast == 1.0f;
        return p.HighlightContrast == 1.0f && p.ShadowContrast == 1.0f && p.DetailStrength == 1.0f;
    }

    // The blur nodes run only when the base layer uses them (UE PostProcessing.cpp:870).
    [[nodiscard]] inline bool LocalExposureNeedsBlur( const LocalExposureParams& p )
    {
        return !IsLocalExposureIdentity( p ) && p.Method == Core::LocalExposureMethod::Bilateral &&
               p.BlurredLumBlend > 0.0f;
    }

    // log2 of the EXPOSED middle grey the base layer is pulled toward. Auto: log2(key) + bias (our key is UE's
    // 0.18 x ExposureCompensation, PostProcessTonemap.usf:419); manual: log2(0.18) + bias (UE cancels the
    // compensation under manual metering, PostProcessLocalExposure.cpp:201-207).
    [[nodiscard]] inline float LocalExposureLogMiddleGrey( float autoExposureKey, float middleGreyBiasEV,
                                                           bool manualExposure )
    {
        const float grey = manualExposure ? 0.18f : std::max( autoExposureKey, 1.0e-4f );
        return std::log2( grey ) + middleGreyBiasEV;
    }

    // Dispatch geometry = UE's default path (r.PostProcessing.QuarterResolutionDownsample 0).
    inline constexpr uint32_t kLocalExposureGridDepth   = 32;  // PostProcessHistogram.cpp:90-93, Common.ush:251
    inline constexpr uint32_t kLocalExposureGridTile    = 128; // output px per cell: half-res 8 threads x 8 texels
    inline constexpr uint32_t kLocalExposureBlurDivisor = 32;  // blurred log lum at 1/32 (half-res chain stage 4)
    inline constexpr float    kLocalExposureSliceEV     = 1.0f; // stops per grid slice (§6 item 2)
    // The slice window: 32 EV centred on the exposed middle grey.
    inline constexpr float kLocalExposureSliceHalfWindowEV =
         0.5f * kLocalExposureGridDepth * kLocalExposureSliceEV;

    [[nodiscard]] inline constexpr uint32_t LocalExposureDivUp( uint32_t value, uint32_t divisor )
    {
        return ( value + divisor - 1 ) / divisor;
    }
    [[nodiscard]] inline glm::uvec3 LocalExposureGridExtent( uint32_t width, uint32_t height )
    {
        return { LocalExposureDivUp( width, kLocalExposureGridTile ),
                 LocalExposureDivUp( height, kLocalExposureGridTile ), kLocalExposureGridDepth };
    }
    [[nodiscard]] inline glm::uvec2 LocalExposureBlurExtent( uint32_t width, uint32_t height )
    {
        return { LocalExposureDivUp( width, kLocalExposureBlurDivisor ),
                 LocalExposureDivUp( height, kLocalExposureBlurDivisor ) };
    }
    // Screen UV -> grid UV: the last cell is partial (UE Histogram.cpp:475-482).
    [[nodiscard]] inline glm::vec2 LocalExposureGridUVScale( uint32_t width, uint32_t height )
    {
        const glm::uvec3 grid = LocalExposureGridExtent( width, height );
        return { float( width ) / float( kLocalExposureGridTile * grid.x ),
                 float( height ) / float( kLocalExposureGridTile * grid.y ) };
    }
    [[nodiscard]] inline glm::vec2 LocalExposureBlurUVScale( uint32_t width, uint32_t height )
    {
        const glm::uvec2 blur = LocalExposureBlurExtent( width, height );
        return { float( width ) / float( kLocalExposureBlurDivisor * blur.x ),
                 float( height ) / float( kLocalExposureBlurDivisor * blur.y ) };
    }
    // UE GetBlurRadius (PostProcessWeightedSampleSum.cpp:189-196): blurWidth x percent x 0.01 x 0.5, in blur
    // texels, X and Y alike; not clamped (§6 item 4).
    [[nodiscard]] inline float LocalExposureBlurRadius( uint32_t blurWidth, float kernelSizePercent )
    {
        return float( blurWidth ) * kernelSizePercent * 0.01f * 0.5f;
    }
    // The log2 luminance at the bottom of slice 0, for a view whose last exposure was @p lastExposure (> 0):
    // slice(logLum) = (logLum - SliceLogMin) / kLocalExposureSliceEV, logLum un-exposed (radiance).
    [[nodiscard]] inline float LocalExposureSliceLogMin( float logMiddleGrey, float lastExposure )
    {
        const float exposure = lastExposure > 0.0f && lastExposure < 3.0e38f ? lastExposure : 1.0f;
        return logMiddleGrey - kLocalExposureSliceHalfWindowEV - std::log2( exposure );
    }

    // ── FUSION (UE 5.4 Local Exposure Method = Fusion; PostProcessLocalExposure.cpp GetLocalExposureParameters,
    // AddLocalExposureFusionPass). The same Highlight / Shadow Contrast Scale fields, read as how far apart the
    // synthetic exposures are: contrast 1 -> 0 EV (identity), 0 -> 6 EV.
    inline constexpr float kLocalExposureFusionMaxEV = 6.0f;
    inline constexpr float kLocalExposureFusionTargetLuminance =
         0.5f;                                                    // r.LocalExposure.ExposureFusion.TargetLuminance
    inline constexpr uint32_t kLocalExposureFusionMaxLevels = 16; // r.LocalExposure.ExposureFusion.NumLevels

    [[nodiscard]] inline bool IsLocalExposureFusion( const LocalExposureParams& p )
    {
        return p.Method == Core::LocalExposureMethod::Fusion;
    }
    // The highlight exposure's scale, 2^-lerp(6, 0, contrast) (< 1: pulls the highlights into range).
    [[nodiscard]] inline float LocalExposureFusionHighlightScale( const LocalExposureParams& p )
    {
        return std::exp2( -kLocalExposureFusionMaxEV * ( 1.0f - p.HighlightContrast ) );
    }
    // The shadow exposure's scale, 2^+lerp(6, 0, contrast) (> 1: lifts the shade).
    [[nodiscard]] inline float LocalExposureFusionShadowScale( const LocalExposureParams& p )
    {
        return std::exp2( kLocalExposureFusionMaxEV * ( 1.0f - p.ShadowContrast ) );
    }
    // UE: clamp(NumLevels, 1, log2(min(w, h)) + 1) — the pyramid runs down to a texel or two.
    [[nodiscard]] inline uint32_t LocalExposureFusionLevels( uint32_t width, uint32_t height )
    {
        const uint32_t smaller = std::max( 1u, std::min( width, height ) );
        const auto     maxMips = static_cast<uint32_t>( std::floor( std::log2( float( smaller ) ) ) ) + 1u;
        return std::clamp( kLocalExposureFusionMaxLevels, 1u, maxMips );
    }
    // Level @p level's size: each level halves, rounding up (a texel j of level i covers 2j, 2j+1 of level i-1).
    [[nodiscard]] inline glm::uvec2 LocalExposureFusionLevelExtent( uint32_t width, uint32_t height,
                                                                    uint32_t level )
    {
        glm::uvec2 size{ std::max( 1u, width ), std::max( 1u, height ) };
        for ( uint32_t i = 0; i < level; ++i )
            size = { LocalExposureDivUp( size.x, 2u ), LocalExposureDivUp( size.y, 2u ) };
        return size;
    }

    // CPU mirror of the shader's LocalExposureLogExposure (Common/LocalExposure.glslh; UE
    // CalculateLogLocalExposure, PostProcessHistogramCommon.ush:279-310): all three log luminances EXPOSED.
    [[nodiscard]] inline float LogLocalExposure( float logLum, float baseLogLum, float logMiddleGrey,
                                                 const LocalExposureParams& p )
    {
        const float detail       = logLum - baseLogLum;
        float       baseCentered = baseLogLum - logMiddleGrey;
        const float contrast     = baseCentered > 0.0f ? p.HighlightContrast : p.ShadowContrast;
        const auto  smooth       = []( float e0, float strength, float x )
        {
            if ( strength >= 1.0f ) // UE: e1 = e0 + 1 / 0 = inf -> smoothstep 0 (§6 item 3)
                return 0.0f;
            const float e1 = e0 + 1.0f / ( 1.0f - strength );
            const float t  = std::clamp( ( x - e0 ) / ( e1 - e0 ), 0.0f, 1.0f );
            return t * t * ( 3.0f - 2.0f * t );
        };
        float m = 0.0f;
        float t = 0.0f;
        if ( baseCentered > 0.0f )
        {
            m = std::max( 0.0f, baseCentered - p.HighlightThreshold );
            t = smooth( p.HighlightThreshold, p.HighlightThresholdStrength, std::abs( baseCentered ) );
        }
        else
        {
            m = std::min( 0.0f, baseCentered + p.ShadowThreshold );
            t = smooth( p.ShadowThreshold, p.ShadowThresholdStrength, std::abs( baseCentered ) );
        }
        const float thresholdOffset = ( baseCentered - m ) * ( 1.0f - t );
        baseCentered -= thresholdOffset;
        const float logLocalLum =
             logMiddleGrey + thresholdOffset + baseCentered * contrast + detail * p.DetailStrength;
        return logLocalLum - logLum;
    }
} // namespace Desert::Graphic
