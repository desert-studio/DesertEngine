#include "ContentGate.hpp"

namespace Desert::Assets
{
    void ContentGate::BeginWorld( uint64_t startedCount )
    {
        m_State               = ContentState::Loading;
        m_Frames              = 0;
        m_QuietStreak         = 0;
        m_StartedAtFrameBegin = startedCount;
        m_Began               = std::chrono::steady_clock::now();
        m_OpenedAfterMs       = 0.0;
    }

    bool ContentGate::Tick( size_t outstanding, uint64_t startedCount )
    {
        if ( m_State == ContentState::Ready )
            return false;

        // BOTH HALVES, EVERY TICK. `startedCount` is monotonic and counts READS rather than requests,
        // which is the granularity this needs: a request for an asset that is already resident starts no
        // read, and treating that as "the frame asked for something" would hold the gate shut forever on
        // a scene whose every reference resolves out of the cache.
        //
        // The tick runs at the TOP of the host's frame, right after the pump, so what the comparison
        // actually says is "the frame that has just been RENDERED started no read", and `outstanding`
        // says "and nothing is in flight now".
        const bool quietFrame = outstanding == 0 && startedCount == m_StartedAtFrameBegin;

        // Sampled for the NEXT tick before the decision below can return: this is the value the frame
        // about to be rendered will be compared against.
        m_StartedAtFrameBegin = startedCount;
        ++m_Frames;
        m_QuietStreak = quietFrame ? m_QuietStreak + 1 : 0;

        // TWO QUIET TICKS IN A ROW, AND ONE IS DEMONSTRABLY NOT ENOUGH.
        //
        // The rule this replaced was `quietFrame && frames >= 2`, and the second clause was there for the
        // right reason -- the first frame is where the renderer ASKS, so a queue that is empty before
        // anything has looked at the scene says only that nobody has looked. But "at least two ticks"
        // and "two quiet ticks" are different statements, and the chain the whole condition exists for
        // slips between them:
        //
        //   tick N-1  the cloud type is in flight            outstanding 1  -> not quiet
        //   tick N    the pump delivered it; nothing has      outstanding 0, started unchanged
        //             yet had a frame in which to want the       -> QUIET, and `frames >= 2` is true,
        //             volume it names                             so the old rule OPENED HERE
        //   frame N   renders, sees the type, asks for the volume
        //   tick N+1  outstanding 1 again -- but too late
        //
        // The old rule was safe only for chains whose next link is requested inside the COMPLETION
        // DELEGATE (the queue is refilled during the pump, before the tick sees it). That is how the
        // cloud kinds happen to be written today, which is exactly why this would have held until
        // somebody wrote a consumer that asks at draw time. A rule that depends on where an unrelated
        // subsystem puts its request is not a rule.
        //
        // The cost is one frame per load, on a loading screen. Measured on this tree: a scene that asks
        // for nothing still opens at the minimum, and Clouds_HeroTrio pays one extra frame of black.
        if ( m_QuietStreak < kQuietFramesToOpen )
            return false;

        m_OpenedAfterMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_Began ).count();
        m_State = ContentState::Ready;
        return true;
    }

    double ContentGate::ElapsedMs() const
    {
        if ( m_State == ContentState::Ready )
            return m_OpenedAfterMs;
        return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_Began ).count();
    }
} // namespace Desert::Assets
