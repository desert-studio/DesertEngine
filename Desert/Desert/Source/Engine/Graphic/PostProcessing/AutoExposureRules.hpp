#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Desert::Graphic
{
    // The auto-exposure histogram's window and its outlier tails, in a header the tests can compile
    // without a renderer — and, more to the point, next to the ONE fact that has to hold about them.
    //
    // THE RELATION. A histogram meter is only honest over the range it can represent. The procedural sky
    // writes the sun disc at up to kSkyLuminanceCeilingMax (SkyLuminanceCeiling) and the meter's top bin has
    // to reach that, or every luminance above the ceiling collapses into one bin and the meter cannot
    // tell the sun from a merely bright sky. Before the physical sun existed nothing in a frame exceeded 4 and
    // a ceiling of 4 was fine; the sky pass then started writing 1000 and the two quietly disagreed by
    // eight stops. Neither side is wrong on its own, which is exactly why it needs an assertion rather
    // than a comment — the defect class this project has paid for most.

    struct AutoExposureWindow
    {
        float MinLogLum;   // log2 of the darkest metered luminance
        float MaxLogLum;   // log2 of the brightest
        float LowPercent;  // discard this fraction of the darkest samples
        float HighPercent; // keep up to this cumulative fraction

        float Range() const
        {
            return MaxLogLum - MinLogLum;
        }

        // Luminance quantisation, in stops per bin. Widening the window trades this away, so a test can
        // state how much drift an ordinary scene is allowed to show.
        float StopsPerBin( uint32_t bins ) const
        {
            return ( bins > 1 ) ? Range() / static_cast<float>( bins - 1 ) : Range();
        }

        bool Covers( float luminance ) const
        {
            return luminance > 0.0f && std::log2( luminance ) <= MaxLogLum;
        }
    };

    // EV100 <-> luminance (cd/m^2), the reflected-light meter law with K = 12.5: L = 2^(EV100 - 3). One law for
    // the exposure range (PostProcessSettings::AutoExposureMin/MaxEV100) and for a light's EV100 units
    // (ECS::LightPhotometry), so an EV means the same brightness wherever it is written.
    inline float EV100ToLuminance( float ev100 )
    {
        return std::exp2( ev100 - 3.0f );
    }
    inline float LuminanceToEV100( float luminance )
    {
        return std::log2( std::max( luminance, 1e-30f ) ) + 3.0f;
    }

    // UE's middle grey: the average a target exposure puts at 0.18.
    inline constexpr float kEyeAdaptationMiddleGrey = 0.18f;

    // THE EXPOSURE LAW, one for auto and manual (UE ExposureScale = 2^Bias x 0.18 / AverageLuminance):
    // compensation = Core::AutoExposureCompensation(AutoExposureBias) = 2^Bias, averageLuminance the metered
    // (auto, clamped to the Min / Max EV100 range) or stated (manual) average. Common/ViewExposure.glslh is its
    // GPU twin over the adapted 1x1 image.
    inline float ExposureScale( float averageLuminance, float compensation )
    {
        return compensation * kEyeAdaptationMiddleGrey / std::max( averageLuminance, 1e-30f );
    }

    // MANUAL EXPOSURE IN EV100 (UE's Manual metering mode). A manual exposure states the EV100 the meter
    // would otherwise have read; the scale is then the auto path's own law with that reading, so a manual EV100
    // and an auto-exposure adapted to the same EV100 give the same picture, and the compensation (UE's Exposure
    // Compensation, AutoExposureBias) offsets both, as UE applies it in Manual too.
    inline float ManualExposureScale( float ev100, float compensation )
    {
        return ExposureScale( EV100ToLuminance( ev100 ), compensation );
    }

    // The smallest exposure scale the auto-exposure can settle on: the meter clamped at its Max EV100.
    inline float LowestAutoExposureScale( float maxEV100, float compensation )
    {
        return ExposureScale( EV100ToLuminance( maxEV100 ), compensation );
    }

    // THE SKY'S ON-SCREEN LUMINANCE CEILING, derived rather than chosen in cd/m^2.
    //
    // The physical sky writes the sun disc at its true luminance (illuminance over its solid angle — ~10^9
    // cd/m^2 for a 100 000 lux sun), which no fp16 target holds and which, held, would saturate every bloom
    // box it touches. UE does not clamp: it stores the scene PRE-EXPOSED, so the disc lands at a fixed number
    // of times display white whatever the light's units. This renderer stores raw luminance, so the ceiling
    // is stated in the same terms instead: kSkyExposedLuminanceCeiling times white after the LOWEST exposure
    // the frame can use (the manual scale, or the meter clamped at Max EV100), and never above
    // kSkyLuminanceCeilingMax, the most the fp16 scene target can take with headroom for one additive pass
    // over it. The former fixed 1000 cd/m^2 is exactly this at exposure 1, and it cut the whole sky of a
    // physical sun (a 100 000 lux sky is ~10^4 cd/m^2).
    inline constexpr float kSkyExposedLuminanceCeiling = 1000.0f;
    inline constexpr float kSkyLuminanceCeilingMax     = 65504.0f * 0.5f; // half the largest finite fp16

    inline float SkyLuminanceCeiling( float lowestExposureScale )
    {
        return std::min( kSkyExposedLuminanceCeiling / std::max( lowestExposureScale, 1e-30f ),
                         kSkyLuminanceCeilingMax );
    }

    // The luminance that reaches the split below is AEHistogram.shader's MeteredLuminance: UE's
    // r.AutoExposure.LuminanceMethod default 0, UNIFORM 1/3 weights (UE 5.8 PostProcessEyeAdaptation.cpp:150-152
    // default, :738-741), floored at the histogram bottom (LuminanceMin = exp2(HistogramLogMin), :693), which
    // the split repeats so black lands in bucket 0 and log2 never sees 0.
    // Where a luminance lands in the histogram: AEHistogram's mapping, ported from UE 5.8
    // PostProcessHistogram.usf:146-165. The continuous position is split LINEARLY between the two buckets that
    // straddle it (Weight1 = frac, Weight0 = 1 - frac), so the metered average moves continuously with the scene
    // instead of stepping a whole bucket. Bucket 0 is black: its Weight0 is scaled by
    // kEyeAdaptationBlackHistogramBucketInfluence (UE's r.EyeAdaptation.BlackHistogramBucketInfluence default 0,
    // PostProcessEyeAdaptation.cpp:110-112), so the editor's empty background does not meter.
    struct AutoExposureBucketSplit
    {
        uint32_t Bucket0;
        uint32_t Bucket1;
        float    Weight1; // Bucket1's share; Bucket0 takes 1 - Weight1
    };
    inline constexpr float         kEyeAdaptationBlackHistogramBucketInfluence = 0.0f;
    inline AutoExposureBucketSplit AutoExposureSplit( const AutoExposureWindow& window, float luminance,
                                                      uint32_t bins )
    {
        if ( bins == 0 )
            return { 0, 0, 0.0f };
        const float    floorLum = std::exp2( window.MinLogLum );
        const float    metered  = ( luminance >= floorLum ) ? luminance : floorLum; // NaN -> the floor
        const uint32_t last     = bins - 1;
        const float    fBucket =
             std::clamp( ( std::log2( metered ) - window.MinLogLum ) / window.Range(), 0.0f, 1.0f ) *
             static_cast<float>( last );
        const uint32_t bucket0 = std::min( static_cast<uint32_t>( fBucket ), last );
        return { bucket0, std::min( bucket0 + 1, last ), fBucket - std::floor( fBucket ) };
    }

    // The bucket carrying the larger share of a luminance: what the tests ask "where does this land".
    inline uint32_t AutoExposureBin( const AutoExposureWindow& window, float luminance, uint32_t bins )
    {
        const AutoExposureBucketSplit split = AutoExposureSplit( window, luminance, bins );
        return ( split.Weight1 >= 0.5f ) ? split.Bucket1 : split.Bucket0;
    }

    // The histogram counts in uint (portable atomics; UE packs weight x 2^19 into thread-local histograms,
    // PostProcessHistogram.usf:15-18). Each texel adds at most 1 x scale, so the scale is the largest that keeps a
    // whole frame of texels inside a uint, capped at 2^16 (finer than any percentile needs).
    inline uint32_t AutoExposureWeightScale( uint64_t texels )
    {
        constexpr uint64_t kCap = 1ull << 16;
        return static_cast<uint32_t>(
             std::clamp<uint64_t>( 0xFFFFFFFFull / std::max<uint64_t>( texels, 1 ), 1, kCap ) );
    }

    // What AEAverage adapts with, derived from a view's auto-exposure settings the way UE derives its eye
    // adaptation parameters. Ported from UE 5.8 Engine/Source/Runtime/Renderer/Private/PostProcess/
    // PostProcessEyeAdaptation.cpp:596-715 (GetEyeAdaptationParameters), adapted: the extended (EV100) luminance
    // range only - UE's project default; the EV100 range is read through this engine's ONE meter law
    // (EV100ToLuminance above, the average luminance an EV100 stands for, shared with the lights' EV100 units)
    // instead of UE's white point x 0.18 (LuminanceMaxFromLensAttenuation) - the two differ by log2(0.18 / 0.125)
    // = 0.53 EV; plain floats in and out so the tests need no renderer; no physical camera, meter mask,
    // compensation curve or debug view.
    struct EyeAdaptationParameters
    {
        float MinAverageLuminance; // the metered average is clamped to [Min, Max] before it becomes a target
        float MaxAverageLuminance;
        float LowPercent;  // fraction of the darkest histogram samples discarded
        float HighPercent; // cumulative fraction kept (the brightest above it discarded)
        float SpeedUp;     // EV per second toward a brighter target
        float SpeedDown;   // EV per second toward a darker target
        float ExponentialUpM;
        float ExponentialDownM;
        float StartDistance; // EV: further than this from the target the adaptation is linear, nearer exponential
        // UE's ForceTarget conditions that come from the settings (PostProcessEyeAdaptation.cpp:717-723): an
        // empty EV100 range (Min >= Max) or a negative speed lands on the target, as Manual does.
        bool ForceTarget;
    };

    // r.EyeAdaptation.ExponentialTransitionDistance's default.
    inline constexpr float         kEyeAdaptationStartDistance = 1.5f;
    inline EyeAdaptationParameters ComputeEyeAdaptationParameters( float minEV100, float maxEV100,
                                                                   float lowPercent, float highPercent,
                                                                   float speedUp, float speedDown )
    {
        constexpr float kPercentToScale = 0.01f;
        const float     high            = std::clamp( highPercent, 1.0f, 99.0f ) * kPercentToScale;
        const float     low = std::min( std::clamp( lowPercent, 1.0f, 99.0f ) * kPercentToScale, high );

        const float maxAverage = EV100ToLuminance( maxEV100 );
        const float minAverage = std::min( EV100ToLuminance( minEV100 ), maxAverage );

        // At t = StartTime the exponential curve's slope equals the linear curve's, so the two join without a
        // kink; UE evaluates the limit at a 60 Hz frame for simplicity, and so does this.
        constexpr float kFrameTimeEps = 1.0f / 60.0f;
        const float     startTimeUp   = kEyeAdaptationStartDistance / std::max( speedUp, 0.001f );
        const float     startTimeDown = kEyeAdaptationStartDistance / std::max( speedDown, 0.001f );
        const float     upM = kFrameTimeEps / ( ( 1.0f - std::exp2( -kFrameTimeEps * speedUp ) ) * startTimeUp );
        const float downM = kFrameTimeEps / ( ( 1.0f - std::exp2( -kFrameTimeEps * speedDown ) ) * startTimeDown );

        const bool validRange  = minEV100 < maxEV100;
        const bool validSpeeds = speedDown >= 0.0f && speedUp >= 0.0f;
        return { minAverage,
                 maxAverage,
                 low,
                 high,
                 speedUp,
                 speedDown,
                 upM,
                 downM,
                 kEyeAdaptationStartDistance,
                 !validRange || !validSpeeds };
    }

    // The luminance a bin index stands for when the average is reconstructed — AEAverage's own formula.
    // Paired with AutoExposureBin so a round-trip test can bound the meter's quantisation error.
    inline float AutoExposureBinLuminance( const AutoExposureWindow& window, uint32_t bin, uint32_t bins )
    {
        const float last = ( bins > 1 ) ? static_cast<float>( bins - 1 ) : 1.0f;
        return std::exp2( window.MinLogLum + ( static_cast<float>( bin ) / last ) * window.Range() );
    }
} // namespace Desert::Graphic
