#include "ContentGate.hpp"

namespace Desert::Assets
{
    void ContentGate::BeginWorld( uint64_t startedCount )
    {
        m_State               = ContentState::Loading;
        m_Frames              = 0;
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
        const bool quietFrame = outstanding == 0 && startedCount == m_StartedAtFrameBegin;

        // Sampled for the NEXT tick before the decision below can return: this is the value the frame
        // about to be rendered will be compared against.
        m_StartedAtFrameBegin = startedCount;
        ++m_Frames;

        if ( m_Frames < kMinimumFrames || !quietFrame )
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
