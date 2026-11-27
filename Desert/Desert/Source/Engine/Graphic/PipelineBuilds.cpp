#include <Engine/Graphic/PipelineBuilds.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>

namespace Desert::Graphic::PipelineBuilds
{
    uint64_t Tracker::OnStarted( const PipelineRole role, const std::string_view name )
    {
        const std::lock_guard lock( m_Mutex );
        ++m_Pending[Index( role )];
        ++m_Started[Index( role )];
        const uint64_t ticket = m_NextTicket++;
        m_InFlight.emplace( ticket, InFlightBuild{ .Name  = std::string( name ),
                                                   .Begin = std::chrono::steady_clock::now(),
                                                   .Role  = role } );
        return ticket;
    }

    void Tracker::OnFinished( const uint64_t ticket )
    {
        {
            const std::lock_guard lock( m_Mutex );
            const auto            found = m_InFlight.find( ticket );
            // A ticket is OnStarted's answer and is finished once; anything else is a defect of the caller.
            assert( found != m_InFlight.end() && "PipelineBuilds: a ticket finished twice or never started" );
            if ( found == m_InFlight.end() )
                return;
            const size_t role = Index( found->second.Role );
            --m_Pending[role];
            ++m_Finished[role];
            m_InFlight.erase( found );
        }
        m_Idle.notify_all();
    }

    size_t Tracker::Pending( const PipelineRole role ) const
    {
        const std::lock_guard lock( m_Mutex );
        return m_Pending[Index( role )];
    }

    uint64_t Tracker::Started( const PipelineRole role ) const
    {
        const std::lock_guard lock( m_Mutex );
        return m_Started[Index( role )];
    }

    uint64_t Tracker::Finished( const PipelineRole role ) const
    {
        const std::lock_guard lock( m_Mutex );
        return m_Finished[Index( role )];
    }

    std::vector<InFlightBuild> Tracker::InFlight( const size_t max ) const
    {
        const std::lock_guard      lock( m_Mutex );
        std::vector<InFlightBuild> oldest;
        oldest.reserve( std::min( max, m_InFlight.size() ) );
        // Tickets rise with every start, so the map's order is the start order: the oldest first.
        for ( const auto& [ticket, build] : m_InFlight )
        {
            if ( oldest.size() == max )
                break;
            oldest.push_back( build );
        }
        return oldest;
    }

    void Tracker::WaitIdle()
    {
        std::unique_lock lock( m_Mutex );
        m_Idle.wait( lock, [this] { return m_Pending[0] == 0 && m_Pending[1] == 0; } );
    }

    void Tracker::WaitUntil( const std::function<bool()>& done )
    {
        // @p done reads pipelines' own state (an atomic each), never this tracker, so it is asked OUTSIDE the
        // lock; an OnFinished between the question and the wait is caught by the bounded wait.
        while ( !done() )
        {
            std::unique_lock lock( m_Mutex );
            m_Idle.wait_for( lock, std::chrono::milliseconds( 10 ) );
        }
    }

    void Tracker::RecordCallerBlock( const std::chrono::nanoseconds blocked )
    {
        const std::lock_guard lock( m_Mutex );
        m_CallerBlocked += blocked;
        if ( blocked > m_CallerBlockedMax )
            m_CallerBlockedMax = blocked;
    }

    Tracker::CallerCost Tracker::CallerBlocked() const
    {
        const std::lock_guard lock( m_Mutex );
        return { m_CallerBlocked, m_CallerBlockedMax };
    }
} // namespace Desert::Graphic::PipelineBuilds
