#pragma once

// WHERE A LONG CALL SAYS WHAT IT IS DOING (GoF Observer: the code doing the work reports it, nobody guesses it
// from outside). Handed down by pointer; a null sink is a call nobody watches. EVERY METHOD IS THREAD-SAFE:
// shader maps are built on the JobSystem and report from the workers.
//
// SP-MODEL CONTRACT. The count of a step is fixed before it counts:
//  - Expect is called AT MOST ONCE per step and BEFORE the step's first BeginItem. A second call, or a call after
//    an item has begun, is refused by the sink (logged once per step); the count on screen never moves.
//  - A call that does several kinds of work names each with BeginStep before that work's Expect and items.

#include <cstdint>
#include <string_view>

namespace Desert::Core
{
    class WorkSink
    {
    public:
        virtual ~WorkSink() = default;

        // The call enters the step @p key of its own work (a key of the watcher's plan, e.g. "scene.read").
        // A sink with a plan (StartupProgress) ends the running phase and begins that one; a key its plan lacks
        // is refused and logged, nothing changes. MAIN THREAD.
        virtual void BeginStep( std::string_view key ) noexcept = 0;
        // @p items is the whole number of items the current step will work through. Once, before its first item.
        virtual void Expect( uint64_t items ) noexcept = 0;
        // An item starts; the returned token finishes it. @p name is copied.
        [[nodiscard]] virtual uint64_t BeginItem( std::string_view name ) noexcept = 0;
        virtual void                   FinishItem( uint64_t token ) noexcept    = 0;
        // A person asked the start to stop (the splash close button): the call stops after the items in hand
        // and says how far it got. Asked between items.
        [[nodiscard]] virtual bool StopRequested() const noexcept = 0;
    };

    // One item for the length of a scope; nothing when @p sink is null.
    class ScopedWorkItem
    {
    public:
        ScopedWorkItem( WorkSink* sink, const std::string_view name ) noexcept
            : m_Sink( sink ), m_Token( sink ? sink->BeginItem( name ) : 0 )
        {
        }
        ~ScopedWorkItem()
        {
            if ( m_Sink )
                m_Sink->FinishItem( m_Token );
        }
        ScopedWorkItem( const ScopedWorkItem& )            = delete;
        ScopedWorkItem& operator=( const ScopedWorkItem& ) = delete;

    private:
        WorkSink* m_Sink;
        uint64_t  m_Token;
    };

    [[nodiscard]] inline bool StopRequested( const WorkSink* sink ) noexcept
    {
        return sink && sink->StopRequested();
    }
} // namespace Desert::Core
