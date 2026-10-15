#pragma once

#include <Common/Core/Math/Rounding.hpp>
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

    // MANUAL EXPOSURE IN EV100 (UE's Manual metering mode). A manual exposure states the EV100 the meter
    // would otherwise have read; the scale is then the auto path's own law with that reading — key over the
    // luminance the EV stands for — so a manual EV100 and an auto-exposure adapted to the same EV100 give the
    // same picture, and the key (UE's Exposure Compensation, in our engine AutoExposureKey) offsets both.
    inline float ManualExposureScale( float ev100, float key )
    {
        return key / EV100ToLuminance( ev100 );
    }

    // The smallest exposure scale the auto-exposure can settle on: the meter clamped at its Max EV100.
    inline float LowestAutoExposureScale( float maxEV100, float key )
    {
        return key / EV100ToLuminance( maxEV100 );
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

    // Which bin a luminance lands in — the histogram shader's mapping, so the tests can ask what the
    // meter actually sees rather than what it was meant to.
    inline uint32_t AutoExposureBin( const AutoExposureWindow& window, float luminance, uint32_t bins )
    {
        if ( bins == 0 )
            return 0;
        if ( !( luminance >= 1e-5f ) )
            return 0; // near-black (and NaN) go in the bottom bin, as the shader does

        const float last = static_cast<float>( bins - 1 );
        const float t    = ( std::log2( luminance ) - window.MinLogLum ) / window.Range();
        return static_cast<uint32_t>( Common::Math::RoundToNearest( std::clamp( t, 0.0f, 1.0f ) * last ) );
    }

    // The luminance a bin index stands for when the average is reconstructed — AEAverage's own formula.
    // Paired with AutoExposureBin so a round-trip test can bound the meter's quantisation error.
    inline float AutoExposureBinLuminance( const AutoExposureWindow& window, uint32_t bin, uint32_t bins )
    {
        const float last = ( bins > 1 ) ? static_cast<float>( bins - 1 ) : 1.0f;
        return std::exp2( window.MinLogLum + ( static_cast<float>( bin ) / last ) * window.Range() );
    }
} // namespace Desert::Graphic
