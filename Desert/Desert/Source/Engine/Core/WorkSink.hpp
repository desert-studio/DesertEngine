#pragma once

// WHERE A LONG CALL SAYS WHAT IT IS DOING (UE FSlowTask: the code doing the work reports it, nobody guesses it
// from outside). Handed down by pointer; a null sink is a call nobody watches. EVERY METHOD IS THREAD-SAFE:
// shader maps are built on the JobSystem and report from the workers.

#include <cstdint>
#include <string_view>

namespace Desert::Core
{
    class WorkSink
    {
    public:
        virtual ~WorkSink() = default;

        // @p items more items this call will work through. Called again when a cache miss adds work (the re-plan).
        virtual void Expect( uint64_t items ) noexcept = 0;
        // An item starts; the returned token finishes it. @p name is copied.
        [[nodiscard]] virtual uint64_t BeginItem( std::string_view name ) noexcept = 0;
        virtual void                   FinishItem( uint64_t token ) noexcept    = 0;
        // A person asked the start to stop (the splash's close button): the call stops after the items in hand
        // and says how far it got. Asked between items (UE FSlowTask::ShouldCancel).
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
