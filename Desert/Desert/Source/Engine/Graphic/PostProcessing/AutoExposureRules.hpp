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
    // writes the sun disc at up to kSkyLuminanceClamp (ProceduralSky.shader) and the meter's top bin has
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

    // The luminance ceiling the procedural sky clamps its output to — mirrored from
    // Editor/Resources/Shaders/Programs/ProceduralSky/ProceduralSky.shader's kSkyLuminanceClamp. The
    // mirror is the point: the test that pins it fails the moment either side moves alone.
    inline constexpr float kSkyLuminanceClamp = 1000.0f;

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
