#include "AnimationClip.hpp"

namespace Desert::Animation
{
    namespace
    {
        bool SameState( const AnimationNotify& a, const AnimationNotify& b )
        {
            // Track is left out on purpose: moving a state to another row is not a new state.
            return a.Name == b.Name && a.Tick == b.Tick && a.DurationTicks == b.DurationTicks;
        }

        bool Contains( const std::vector<AnimationNotify>& list, const AnimationNotify& notify )
        {
            return std::any_of( list.begin(), list.end(),
                                [&notify]( const AnimationNotify& other ) { return SameState( other, notify ); } );
        }
    } // namespace

    void StepNotifyStates( const std::vector<AnimationNotify>& notifies, std::vector<AnimationNotify>& active,
                           const double before, const double after, const bool forwardPlayback, const bool looped,
                           std::vector<NotifyEvent>& out )
    {
        std::vector<AnimationNotify> now;
        for ( const auto& notify : notifies )
        {
            if ( NotifyStateActiveAt( notify, after ) )
            {
                now.push_back( notify );
            }
        }

        for ( const auto& was : active )
        {
            if ( !Contains( now, was ) )
            {
                out.push_back( NotifyEvent{ was.Name, NotifyEventKind::End } );
            }
        }

        for ( const auto& notify : notifies )
        {
            const bool crossed = forwardPlayback &&
                                 NotifyCrossed( static_cast<double>( notify.Tick.Value ), before, after, looped );
            if ( !notify.IsState() )
            {
                if ( crossed )
                {
                    out.push_back( NotifyEvent{ notify.Name, NotifyEventKind::Fire } );
                }
                continue;
            }
            const bool isActive  = Contains( now, notify );
            const bool wasActive = Contains( active, notify );
            if ( isActive && !wasActive )
            {
                out.push_back( NotifyEvent{ notify.Name, NotifyEventKind::Begin } );
            }
            else if ( !isActive && !wasActive && crossed )
            {
                // Entered and left inside one step (a state shorter than the frame): a script still hears
                // both halves, in order, rather than neither.
                out.push_back( NotifyEvent{ notify.Name, NotifyEventKind::Begin } );
                out.push_back( NotifyEvent{ notify.Name, NotifyEventKind::End } );
            }
        }

        active = std::move( now );
    }

    float AnimationCurve::Evaluate( const FrameTime at, const FrameRate tickRate ) const
    {
        if ( Keys.empty() )
        {
            return 0.0F;
        }
        const double t = at.AsTicks();
        // The first key strictly AFTER the sample: a sample exactly on a key, or a sub-tick past it, is
        // then inside the segment that key starts — never a factor above 1 in the one before it.
        const auto next = std::upper_bound( Keys.begin(), Keys.end(), t, []( double tick, const ScalarKey& key )
                                            { return tick < static_cast<double>( key.Tick.Value ); } );
        if ( next == Keys.begin() )
        {
            return Keys.front().Value;
        }
        if ( next == Keys.end() )
        {
            return Keys.back().Value;
        }
        const auto   prev   = next - 1;
        const auto   span   = static_cast<double>( next->Tick.Value - prev->Tick.Value );
        const auto   factor = static_cast<float>( ( t - static_cast<double>( prev->Tick.Value ) ) / span );
        const double spanSeconds =
             span * static_cast<double>( tickRate.Denominator ) / static_cast<double>( tickRate.Numerator );
        return EvaluateSegment( prev->Value, prev->LeaveTangent, next->Value, next->ArriveTangent, next->Interp,
                                spanSeconds, factor );
    }
} // namespace Desert::Animation
