#include "TimeModel.hpp"

#include <cmath>

namespace Desert::Animation
{
    namespace
    {
        /// `value` clamped into the range an int32 tick can hold. A clip long enough to overflow this at
        /// 24000 ticks per second is 24.8 days, so the clamp is a guard against a corrupt file rather than
        /// a limit anybody can reach by authoring.
        [[nodiscard]] int32_t SaturateToTick( double value )
        {
            constexpr double LOWEST  = -2147483648.0;
            constexpr double HIGHEST = 2147483647.0;
            // NaN IS NOT A POSITION AND INFINITY IS. A NaN has no place on the grid at all, so it becomes
            // the start — the one tick every clip has. An infinity is past the end in a known direction,
            // so it saturates there; that keeps the function MONOTONE, which is what a caller comparing
            // two times relies on even when one of them came out of a corrupt file.
            if ( std::isnan( value ) )
            {
                return 0;
            }
            if ( value <= LOWEST )
            {
                return -2147483648;
            }
            if ( value >= HIGHEST )
            {
                return 2147483647;
            }
            return static_cast<int32_t>( value );
        }
    } // namespace

    TickConversion ConvertTick( FrameNumber tick, FrameRate from, FrameRate to )
    {
        TickConversion out;
        if ( !from.IsValid() || !to.IsValid() )
        {
            // A rate of zero is not a slow clock, it is a missing one. Reporting the tick unchanged and
            // NOT exact is the only answer that cannot be mistaken for a conversion that happened.
            out.Ticks = tick;
            out.Exact = false;
            return out;
        }

        // ticks_to = tick * (to.Num / to.Den) / (from.Num / from.Den)
        //          = tick * to.Num * from.Den / (to.Den * from.Num)
        // In 64-bit integers, so the exactness question is answered by a remainder rather than by
        // comparing two doubles.
        const int64_t numerator   = static_cast<int64_t>( tick.Value ) * to.Numerator * from.Denominator;
        const int64_t denominator = static_cast<int64_t>( to.Denominator ) * from.Numerator;

        const int64_t quotient  = numerator / denominator;
        const int64_t remainder = numerator % denominator;

        if ( remainder == 0 )
        {
            out.Ticks = FrameNumber{ SaturateToTick( static_cast<double>( quotient ) ) };
            out.Exact = true;
            return out;
        }

        // Round half away from zero, so a key at exactly half a destination tick does not depend on the
        // sign of the time it was authored at.
        const int64_t twiceRemainder = ( remainder < 0 ? -remainder : remainder ) * 2;
        const int64_t absDenominator = denominator < 0 ? -denominator : denominator;
        const int64_t step           = ( numerator < 0 ) == ( denominator < 0 ) ? 1 : -1;
        const int64_t rounded        = twiceRemainder >= absDenominator ? quotient + step : quotient;

        out.Ticks = FrameNumber{ SaturateToTick( static_cast<double>( rounded ) ) };
        out.Exact = false;

        // How far the rounded tick sits from the true value, in millionths of a destination tick.
        const double exactValue = static_cast<double>( numerator ) / static_cast<double>( denominator );
        out.RoundedAwayMicro =
             static_cast<int64_t>( std::llround( ( static_cast<double>( rounded ) - exactValue ) * 1.0e6 ) );
        return out;
    }

    FrameTime SecondsToFrameTime( double seconds, FrameRate rate )
    {
        FrameTime out;
        if ( !rate.IsValid() )
        {
            return out;
        }

        const double ticks = seconds * rate.AsDouble();
        if ( !std::isfinite( ticks ) )
        {
            // Saturate (or land on 0 for a NaN) and carry NO subframe: `floor(inf)` is `inf` and
            // `inf - inf` is a NaN, so computing the fraction first would put a NaN in the float this
            // type promises to keep in [0, 1).
            out.Frame = FrameNumber{ SaturateToTick( ticks ) };
            return out;
        }

        const double whole = std::floor( ticks );
        out.Frame          = FrameNumber{ SaturateToTick( whole ) };
        out.Subframe       = static_cast<float>( ticks - whole );
        return out;
    }

    double FrameTimeToSeconds( FrameTime time, FrameRate rate )
    {
        if ( !rate.IsValid() )
        {
            return 0.0;
        }
        return time.AsTicks() * static_cast<double>( rate.Denominator ) / static_cast<double>( rate.Numerator );
    }

    FrameNumber NearestTick( FrameTime time )
    {
        return FrameNumber{ SaturateToTick( std::round( time.AsTicks() ) ) };
    }

    FrameTime AdvanceFrameTime( FrameTime time, double seconds, FrameRate rate )
    {
        if ( !rate.IsValid() || !std::isfinite( seconds ) )
        {
            return time;
        }

        const double advanced = static_cast<double>( time.Subframe ) + seconds * rate.AsDouble();
        if ( !std::isfinite( advanced ) )
        {
            return time;
        }
        const double whole = std::floor( advanced );

        FrameTime out;
        out.Frame    = FrameNumber{ SaturateToTick( static_cast<double>( time.Frame.Value ) + whole ) };
        out.Subframe = static_cast<float>( advanced - whole );
        return out;
    }

    FrameTime WrapFrameTime( FrameTime time, FrameNumber duration )
    {
        if ( duration.Value <= 0 )
        {
            // A clip of no length has exactly one position, and it is the start. Returning `time` would
            // hand back a tick outside a clip that has no inside.
            return FrameTime{};
        }

        FrameTime out = time;
        out.Frame     = FrameNumber{ time.Frame.Value % duration.Value };
        if ( out.Frame.Value < 0 )
        {
            out.Frame.Value += duration.Value;
        }
        return out;
    }

    FrameNumber SnapToDisplayRate( FrameTime time, FrameRate tickRate, FrameRate displayRate )
    {
        if ( !tickRate.IsValid() || !displayRate.IsValid() || tickRate == displayRate )
        {
            return time.Frame;
        }

        // Ticks per display frame, as a rational: (tickRate / displayRate).
        const double ticksPerDisplayFrame = tickRate.AsDouble() / displayRate.AsDouble();
        if ( ticksPerDisplayFrame <= 0.0 )
        {
            return time.Frame;
        }

        const double displayFrame = std::round( time.AsTicks() / ticksPerDisplayFrame );
        return FrameNumber{ SaturateToTick( std::round( displayFrame * ticksPerDisplayFrame ) ) };
    }

    int32_t DisplayFrameIndex( FrameNumber tick, FrameRate tickRate, FrameRate displayRate )
    {
        if ( !tickRate.IsValid() || !displayRate.IsValid() )
        {
            return 0;
        }
        const double ticksPerDisplayFrame = tickRate.AsDouble() / displayRate.AsDouble();
        if ( ticksPerDisplayFrame <= 0.0 )
        {
            return 0;
        }
        return SaturateToTick( std::floor( static_cast<double>( tick.Value ) / ticksPerDisplayFrame ) );
    }
} // namespace Desert::Animation
