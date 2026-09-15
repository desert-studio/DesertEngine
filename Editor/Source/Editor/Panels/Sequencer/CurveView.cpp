#include "CurveView.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::Editor::Sequencer
{
    namespace
    {
        /// A span that cannot be zero, so no mapping below divides by one.
        [[nodiscard]] double SafeSpan( double span )
        {
            return std::abs( span ) < 1e-9 ? 1e-9 : span;
        }
    } // namespace

    double CurveViewport::PixelsPerSecond() const
    {
        return static_cast<double>( X1 - X0 ) / SafeSpan( TimeEnd - TimeStart );
    }

    float CurveViewport::PixelsPerValue() const
    {
        // NEGATIVE IS NOT A BUG HERE AND IS NOT RETURNED. Y grows downward, so a value gains pixels
        // upward; callers want the magnitude, and the one place the sign matters is ValueToY below.
        return static_cast<float>( static_cast<double>( Y1 - Y0 ) /
                                   SafeSpan( static_cast<double>( ValueMax ) - static_cast<double>( ValueMin ) ) );
    }

    float CurveViewport::TimeToX( double seconds ) const
    {
        return X0 + static_cast<float>( ( seconds - TimeStart ) * PixelsPerSecond() );
    }

    double CurveViewport::XToTime( float x ) const
    {
        return TimeStart + static_cast<double>( x - X0 ) / SafeSpan( PixelsPerSecond() );
    }

    float CurveViewport::ValueToY( float value ) const
    {
        // Y1 is the BOTTOM and holds ValueMin, so the value climbs towards Y0.
        return Y1 - static_cast<float>( ( static_cast<double>( value ) - static_cast<double>( ValueMin ) ) *
                                        static_cast<double>( PixelsPerValue() ) );
    }

    float CurveViewport::YToValue( float y ) const
    {
        return ValueMin + static_cast<float>( static_cast<double>( Y1 - y ) /
                                              SafeSpan( static_cast<double>( PixelsPerValue() ) ) );
    }

    glm::vec2 CurveViewport::HandleOffset( float slope, float handlePixels, bool leaving ) const
    {
        // One second of curve time, and the rise that slope produces over it, both in pixels. The minus is
        // the Y flip: a POSITIVE slope must point UP the screen.
        const auto  dx = static_cast<float>( PixelsPerSecond() );
        const float dy = -slope * PixelsPerValue();

        const float length = std::sqrt( dx * dx + dy * dy );
        if ( length < 1e-6F )
        {
            return { 0.0F, 0.0F };
        }

        const float sign = leaving ? 1.0F : -1.0F;
        return { sign * dx / length * handlePixels, sign * dy / length * handlePixels };
    }

    std::optional<float> CurveViewport::SlopeFromHandle( glm::vec2 offsetPixels, bool leaving ) const
    {
        const float dx = leaving ? offsetPixels.x : -offsetPixels.x;
        const float dy = leaving ? offsetPixels.y : -offsetPixels.y;

        // A handle at or behind its key says nothing a slope can express — see the header.
        if ( dx <= 0.0F )
        {
            return std::nullopt;
        }

        const float pixelsPerValue = PixelsPerValue();
        if ( std::abs( pixelsPerValue ) < 1e-9F )
        {
            return std::nullopt;
        }

        const double seconds    = static_cast<double>( dx ) / SafeSpan( PixelsPerSecond() );
        const double valueUnits = static_cast<double>( -dy ) / static_cast<double>( pixelsPerValue );
        return static_cast<float>( valueUnits / SafeSpan( seconds ) );
    }

    glm::vec2 FitValueRange( const std::vector<Animation::ScalarKey>& keys, float paddingFraction )
    {
        if ( keys.empty() )
        {
            return { -1.0F, 1.0F };
        }

        float low  = keys.front().Value;
        float high = keys.front().Value;
        for ( const Animation::ScalarKey& key : keys )
        {
            low  = std::min( low, key.Value );
            high = std::max( high, key.Value );
        }

        // THE FLAT CHANNEL, which is most of a clip. A unit window around the value, so the line is drawn
        // through the middle of the box instead of through a division by zero.
        if ( high - low < 1e-6F )
        {
            return { low - 1.0F, high + 1.0F };
        }

        const float pad = ( high - low ) * std::max( 0.0F, paddingFraction );
        return { low - pad, high + pad };
    }

    int32_t ChooseFrameStep( double pixelsPerFrame, float minPixels )
    {
        if ( !( pixelsPerFrame > 0.0 ) || !( minPixels > 0.0F ) )
        {
            return 1;
        }

        // The 1-2-5-10 ladder, so every label an animator reads is a round frame number. Climbing by
        // decade rather than doubling is what keeps 25 from becoming the gridline spacing on a 25 fps clip.
        int32_t    step  = 1;
        int32_t    cycle = 0;
        const auto want  = static_cast<double>( minPixels );
        while ( static_cast<double>( step ) * pixelsPerFrame < want && step < 1000000 )
        {
            switch ( cycle % 3 )
            {
                case 0:
                    step *= 2;
                    break;
                case 1:
                    step = step / 2 * 5;
                    break;
                default:
                    step *= 2;
                    break;
            }
            ++cycle;
        }
        return std::max( 1, step );
    }
} // namespace Desert::Editor::Sequencer
