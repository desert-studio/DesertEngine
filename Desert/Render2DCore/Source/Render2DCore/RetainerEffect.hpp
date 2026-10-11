#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace Desert::Graphic::Render2D
{
    // The composite of one retained layer (UE Retainer Box + its effect material), as data. The walk fills
    // it from UIRetainerData in SCREEN px (canvas scale applied) and the view clock; Render2D hands it to
    // UIRetainer.shader as push constants. The functions below are that shader's math in C++, line for
    // line, so a suite can state what a pixel does without a device.
    struct RetainerEffect
    {
        float Opacity = 1.0f;

        bool Mask       = false;
        bool InvertMask = false;

        bool  Haze          = false;
        float HazeAmplitude = 0.0f; // screen px
        float HazeScale     = 1.0f; // screen px per noise cell
        float HazeSpeed     = 0.0f; // cells per second
        float Time          = 0.0f; // the view clock (UIViewContext::Time; the host step under --render-movie)

        [[nodiscard]] bool operator==( const RetainerEffect& ) const = default;
    };

    // Mask alpha at a pixel -> how much of the layer shows there.
    [[nodiscard]] inline float RetainerMaskCoverage( float maskAlpha, bool invert )
    {
        const float a = glm::clamp( maskAlpha, 0.0f, 1.0f );
        return invert ? 1.0f - a : a;
    }

    // Integer lattice hash -> [0,1). UIRetainer.shader uses the same constants on uint, so the two agree
    // bit for bit on the lattice values.
    [[nodiscard]] inline float RetainerHash( int32_t x, int32_t y )
    {
        uint32_t h = static_cast<uint32_t>( x ) * 374761393u + static_cast<uint32_t>( y ) * 668265263u;
        h          = ( h ^ ( h >> 13u ) ) * 1274126177u;
        h ^= h >> 16u;
        return static_cast<float>( h & 0x00FFFFFFu ) / 16777216.0f;
    }

    // Smooth value noise in [0,1).
    [[nodiscard]] inline float RetainerNoise( const glm::vec2& p )
    {
        const glm::vec2 i = glm::floor( p );
        const glm::vec2 f = p - i;
        const glm::vec2 u = f * f * ( 3.0f - 2.0f * f );
        const auto      x = static_cast<int32_t>( i.x );
        const auto      y = static_cast<int32_t>( i.y );
        const float     a = RetainerHash( x, y );
        const float     b = RetainerHash( x + 1, y );
        const float     c = RetainerHash( x, y + 1 );
        const float     d = RetainerHash( x + 1, y + 1 );
        return glm::mix( glm::mix( a, b, u.x ), glm::mix( c, d, u.x ), u.y );
    }

    // Where the haze samples the layer from, in layer px, for the pixel at @p localPx. A pure function of
    // the pixel and the clock: two runs at the same fixed step displace every pixel identically. The cells
    // rise (−y) with time, which is what hot air over sand does.
    [[nodiscard]] inline glm::vec2 RetainerHazeOffsetPx( const RetainerEffect& fx, const glm::vec2& localPx )
    {
        if ( !fx.Haze || fx.HazeAmplitude <= 0.0f || fx.HazeScale <= 0.0f )
            return glm::vec2( 0.0f );
        const glm::vec2 p = localPx / fx.HazeScale + glm::vec2( 0.0f, fx.Time * fx.HazeSpeed );
        const glm::vec2 n( RetainerNoise( p ), RetainerNoise( p + glm::vec2( 17.0f, 5.0f ) ) );
        return ( n * 2.0f - 1.0f ) * fx.HazeAmplitude;
    }
} // namespace Desert::Graphic::Render2D
