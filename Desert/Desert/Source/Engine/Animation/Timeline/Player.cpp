#include "Player.hpp"

#include <rflcpp/rfl/enums.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace Desert::Animation::Timeline
{
    Player::Player( const FrameRate tickRate, const FrameNumber start, const FrameNumber end )
         : m_TickRate( tickRate ), m_Start( start ), m_End( std::max( start, end ) ), m_Current{ start, 0.0F }
    {
    }

    void Player::Play()
    {
        // UE's rule: Play on a finished one-shot starts it again rather than finishing on the spot. A
        // one-shot that ended stands on the end it was travelling to.
        if ( m_State == PlayState::Stopped && m_Loop == LoopMode::Once )
        {
            const bool forward = m_Rate * m_Direction >= 0.0;
            if ( forward && m_Current.Frame == m_End && m_Current.Subframe == 0.0F )
            {
                m_Current = FrameTime{ m_Start, 0.0F };
            }
            else if ( !forward && m_Current.Frame == m_Start && m_Current.Subframe == 0.0F )
            {
                m_Current = FrameTime{ m_End, 0.0F };
            }
        }
        m_State = PlayState::Playing;
    }

    void Player::Pause()
    {
        if ( m_State == PlayState::Playing )
        {
            m_State = PlayState::Paused;
        }
    }

    void Player::Stop()
    {
        m_Current   = FrameTime{ m_Start, 0.0F };
        m_Direction = 1;
        m_State     = PlayState::Stopped;
    }

    void Player::SetLoopMode( const LoopMode mode )
    {
        m_Loop = mode;
        if ( mode != LoopMode::PingPong )
        {
            // Only PingPong turns round; leaving it must not leave the player running backwards.
            m_Direction = 1;
        }
    }

    void Player::SetPlayRate( const double rate )
    {
        m_Rate = std::isfinite( rate ) ? rate : 0.0;
    }

    TimeStep Player::JumpTo( const FrameTime at )
    {
        const auto start = static_cast<double>( m_Start.Value );
        const auto end   = static_cast<double>( m_End.Value );
        m_Current        = FrameTime::FromTicks( std::clamp( at.AsTicks(), start, end ) );
        return TimeStep{ m_Current, m_Current };
    }

    TimeStep Player::Advance( const double seconds )
    {
        TimeStep step{ m_Current, m_Current };
        if ( m_State != PlayState::Playing || !m_TickRate.IsValid() || !std::isfinite( seconds ) )
        {
            return step;
        }

        const auto   start  = static_cast<double>( m_Start.Value );
        const auto   end    = static_cast<double>( m_End.Value );
        const double length = end - start;
        if ( length <= 0.0 )
        {
            // A range with no inside has one position; a one-shot over it is over at once.
            if ( m_Loop == LoopMode::Once )
            {
                step.Finished = true;
                m_State       = PlayState::Stopped;
            }
            return step;
        }

        // AdvanceFrameTime carries the whole ticks in the integer; the range logic below is on real ticks,
        // bounded by one range length, so the double holds it exactly enough and the result goes back
        // through FrameTime::FromTicks.
        const double    signedSeconds = seconds * m_Rate * m_Direction;
        const FrameTime moved         = AdvanceFrameTime( m_Current, signedSeconds, m_TickRate );
        step.Direction                = signedSeconds < 0.0 ? PlayDirection::Backward : PlayDirection::Forward;
        double t                      = moved.AsTicks();

        if ( t > end || t < start )
        {
            const bool   forward  = t > end;
            const double overflow = forward ? t - end : start - t;
            switch ( m_Loop )
            {
                case LoopMode::Once:
                    t             = forward ? end : start;
                    step.Finished = true;
                    m_State       = PlayState::Stopped;
                    break;

                case LoopMode::Loop:
                {
                    // ONE WRAP PER STEP: a step longer than the range lands where the modulo puts it and
                    // reports a single wrap — the events of the skipped whole loops are not replayed.
                    const double into = std::fmod( overflow, length );
                    t                 = forward ? start + into : end - into;
                    step.Wrapped      = true;
                    break;
                }

                case LoopMode::PingPong:
                {
                    // Reflect off the end once; a step longer than the range stops on the far end.
                    const double back = std::min( overflow, length );
                    t                 = forward ? end - back : start + back;
                    m_Direction       = -m_Direction;
                    step.Reversed     = true;
                    break;
                }
            }
        }

        m_Current = FrameTime::FromTicks( std::clamp( t, start, end ) );
        step.To   = m_Current;
        return step;
    }

    // ToString indexes kLoopModes by the enumerator's value: the list must stay in stored (value) order.
    static_assert(
         []
         {
             for ( std::size_t i = 0; i < kLoopModes.size(); ++i )
                 if ( static_cast<std::size_t>( kLoopModes[i] ) != i )
                     return false;
             return true;
         }(),
         "kLoopModes must list every LoopMode in value order" );

    const char* ToString( const LoopMode mode )
    {
        // The reflected names, taken once: reflection is the only spelling, this keeps a stable pointer to it.
        static const std::array<std::string, kLoopModes.size()> kNames = []
        {
            std::array<std::string, kLoopModes.size()> names;
            for ( std::size_t i = 0; i < kLoopModes.size(); ++i )
                names[i] = rfl::enum_to_string( kLoopModes[i] );
            return names;
        }();
        const auto index = static_cast<std::size_t>( mode );
        return index < kNames.size() ? kNames[index].c_str() : "Unknown";
    }

    std::optional<LoopMode> LoopModeFromString( const std::string_view name )
    {
        const auto parsed = rfl::string_to_enum<LoopMode>( std::string( name ) );
        if ( !parsed )
            return std::nullopt;
        return parsed.value();
    }
} // namespace Desert::Animation::Timeline
