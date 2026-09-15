#include "KeyInterpolation.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::Animation
{
    namespace
    {
        /// A cubic Bezier through two values with control points at one third of the span — six lerps, and
        /// the same construction UE's `BezierInterp` uses (`CurveEvaluation.h:25-36`). The thirds are what
        /// make the control points express a SLOPE: the curve leaves p0 heading at p1 and arrives at p3
        /// from p2, so a tangent scaled by span/3 is exactly the offset that produces it.
        [[nodiscard]] float BezierInterp( float p0, float p1, float p2, float p3, float t )
        {
            const float a = std::lerp( p0, p1, t );
            const float b = std::lerp( p1, p2, t );
            const float c = std::lerp( p2, p3, t );
            const float d = std::lerp( a, b, t );
            const float e = std::lerp( b, c, t );
            return std::lerp( d, e, t );
        }

        /// Seconds between two ticks on `rate`. Negative or zero for keys out of order, which every caller
        /// here treats as "no segment" rather than dividing by it.
        [[nodiscard]] double SpanSeconds( FrameNumber from, FrameNumber to, FrameRate rate )
        {
            if ( !rate.IsValid() )
            {
                return 0.0;
            }
            return static_cast<double>( to.Value - from.Value ) * static_cast<double>( rate.Denominator ) /
                   static_cast<double>( rate.Numerator );
        }
    } // namespace

    const char* ToString( KeyInterp interp )
    {
        switch ( interp )
        {
            case KeyInterp::Constant:
                return "Constant";
            case KeyInterp::Linear:
                return "Linear";
            case KeyInterp::Cubic:
                return "Cubic";
        }
        return "?";
    }

    const char* ToString( TangentMode mode )
    {
        switch ( mode )
        {
            case TangentMode::Auto:
                return "Auto";
            case TangentMode::User:
                return "User";
            case TangentMode::Break:
                return "Break";
        }
        return "?";
    }

    float EvaluateSegment( float startValue, float startLeaveTangent, float endValue, float endArriveTangent,
                           KeyInterp interp, double spanSeconds, float t )
    {
        switch ( interp )
        {
            case KeyInterp::Constant:
                // HOLD, and hold the whole way: the value steps at the later key, not half way to it. A
                // stepped channel whose step lands in the middle of the segment is a rounding, not a hold.
                return startValue;

            case KeyInterp::Linear:
                return std::lerp( startValue, endValue, t );

            case KeyInterp::Cubic:
            {
                if ( !( spanSeconds > 0.0 ) )
                {
                    // Two keys on the same tick, or out of order. There is no curve between them and the
                    // later value is what a sampler at that tick means — the same answer the linear path
                    // gives for the same input, so the two cannot disagree.
                    return endValue;
                }

                // The control points, in VALUE units: a tangent is value-per-second, and one third of the
                // span in seconds is the distance the control point sits along the segment.
                const auto  third = static_cast<float>( spanSeconds / 3.0 );
                const float p1    = startValue + startLeaveTangent * third;
                const float p2    = endValue - endArriveTangent * third;
                return BezierInterp( startValue, p1, p2, endValue, t );
            }
        }
        return startValue;
    }

    void AutoSetTangents( std::vector<ScalarKey>& keys, FrameRate tickRate )
    {
        if ( keys.empty() || !tickRate.IsValid() )
        {
            return;
        }

        for ( std::size_t i = 0; i < keys.size(); ++i )
        {
            ScalarKey& key = keys[i];
            if ( key.Mode != TangentMode::Auto )
            {
                // The animator set this one. An auto pass that overwrote it would undo their work on the
                // next unrelated edit anywhere in the channel.
                continue;
            }

            const bool isFirst = i == 0;
            const bool isLast  = i + 1 == keys.size();
            if ( isFirst || isLast )
            {
                // FLAT ENDPOINTS. A curve that leaves its last key with a slope overshoots past the end of
                // the clip, where there is no next key to come back from.
                key.ArriveTangent = 0.0F;
                key.LeaveTangent  = 0.0F;
                continue;
            }

            const ScalarKey& previous = keys[i - 1];
            const ScalarKey& next     = keys[i + 1];

            const double spanBefore = SpanSeconds( previous.Tick, key.Tick, tickRate );
            const double spanAfter  = SpanSeconds( key.Tick, next.Tick, tickRate );
            if ( !( spanBefore > 0.0 ) || !( spanAfter > 0.0 ) )
            {
                // Keys sharing a tick have no secant between them; flat is the only slope that cannot be
                // an infinity here.
                key.ArriveTangent = 0.0F;
                key.LeaveTangent  = 0.0F;
                continue;
            }

            const auto secantBefore = static_cast<float>(
                 ( static_cast<double>( key.Value ) - static_cast<double>( previous.Value ) ) / spanBefore );
            const auto secantAfter = static_cast<float>(
                 ( static_cast<double>( next.Value ) - static_cast<double>( key.Value ) ) / spanAfter );

            if ( secantBefore * secantAfter <= 0.0F )
            {
                // A LOCAL EXTREMUM IS FLAT. Without this a peak becomes an overshoot: the curve sails past
                // the highest value the animator authored and comes back, which is the curve-editor bug
                // everybody reports first.
                key.ArriveTangent = 0.0F;
                key.LeaveTangent  = 0.0F;
                continue;
            }

            // The slope through the neighbours, then the MONOTONE CLAMP: three times the smaller adjacent
            // secant is the Fritsch-Carlson limit, and it is what stops a segment between two keys from
            // leaving the interval those two keys bound.
            const float average = ( secantBefore + secantAfter ) * 0.5F;
            const float limit   = 3.0F * std::min( std::fabs( secantBefore ), std::fabs( secantAfter ) );
            const float clamped = std::clamp( average, -limit, limit );

            key.ArriveTangent = clamped;
            key.LeaveTangent  = clamped;
        }
    }

    float SlopeAt( const std::vector<ScalarKey>& keys, FrameNumber tick, FrameRate tickRate )
    {
        if ( keys.size() < 2 || !tickRate.IsValid() )
        {
            return 0.0F;
        }

        // Outside the keyed range the channel is constant, and a constant has no slope to seed from.
        if ( tick <= keys.front().Tick || tick >= keys.back().Tick )
        {
            return 0.0F;
        }

        const auto after = std::lower_bound( keys.begin(), keys.end(), tick,
                                             []( const ScalarKey& key, FrameNumber at )
                                             { return key.Tick < at; } );
        if ( after == keys.begin() || after == keys.end() )
        {
            return 0.0F;
        }
        const auto before = after - 1;

        const double span = SpanSeconds( before->Tick, after->Tick, tickRate );
        if ( !( span > 0.0 ) )
        {
            return 0.0F;
        }

        const auto secant = static_cast<float>(
             ( static_cast<double>( after->Value ) - static_cast<double>( before->Value ) ) / span );

        switch ( after->Interp )
        {
            case KeyInterp::Constant:
                // A hold has no slope. Seeding a key here with anything else would tilt a segment the
                // animator deliberately made flat.
                return 0.0F;

            case KeyInterp::Linear:
                return secant;

            case KeyInterp::Cubic:
                break;
        }

        // THE CURVE'S OWN SLOPE, not the secant between the two keys — "seed the tangent from the current
        // curve slope" (§936) means the curve, and on a cubic those two numbers differ everywhere except
        // the one point where the chord is parallel to the tangent. Using the secant would tilt the new
        // key away from the shape it was inserted into; using the derivative makes the slope continuous
        // across the insertion.
        //
        // It does NOT make the split exact: subdividing a cubic exactly would also change the two
        // NEIGHBOURS' tangents, and an insertion that rewrote its neighbours is a different feature. What
        // is exact is the value at the inserted tick, which is what "the pose does not move" means at the
        // moment the animator is looking at. Tests/Engine/KeyInterpolation measures the residual either
        // side rather than claiming it is zero.
        const auto  third = static_cast<float>( span / 3.0 );
        const float p0    = before->Value;
        const float p1    = before->Value + before->LeaveTangent * third;
        const float p2    = after->Value - after->ArriveTangent * third;
        const float p3    = after->Value;

        const auto  t = static_cast<float>( static_cast<double>( tick.Value - before->Tick.Value ) /
                                           static_cast<double>( after->Tick.Value - before->Tick.Value ) );
        const float u = 1.0F - t;

        // dB/dt of a cubic Bezier, then dt -> seconds. The 3x is the Bezier derivative's own factor.
        const float derivative =
             3.0F * ( u * u * ( p1 - p0 ) + 2.0F * u * t * ( p2 - p1 ) + t * t * ( p3 - p2 ) );
        return static_cast<float>( static_cast<double>( derivative ) / span );
    }
} // namespace Desert::Animation
