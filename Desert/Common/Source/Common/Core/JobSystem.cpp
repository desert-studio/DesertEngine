#include "JobSystem.hpp"

#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/Logger.hpp>

// Optick is on the shipping link line and contributes nothing to it, because after the two guards below
// no translation unit in this engine references a single Optick symbol — the profiling macros are empty
// (Common/Core/Profiler.hpp) and these are the only other two uses in the tree. That absence is not
// asserted by reading the build scripts; scripts/CI/ShippingSymbols.sh reads the linked binary.
#include <optick.h>

#include <algorithm>
#include <exception>
#include <memory>

namespace Common
{
    JobSystem& JobSystem::Get()
    {
        static JobSystem s_Instance;
        return s_Instance;
    }

    JobSystem::JobSystem()
    {
        // FORCE OPTICK'S OWN SINGLETON INTO EXISTENCE BEFORE THE FIRST WORKER DOES, and the reason is
        // static destruction order, not profiling. `Optick::Core` is a function-local static, so it is
        // destroyed in reverse order of when its construction COMPLETED. Left to itself, the first worker
        // thread's OPTICK_THREAD is what creates it — after this constructor has returned — so Core is
        // destroyed BEFORE this pool is, and the join in ~JobSystem then unwinds nine ThreadScopes into a
        // dead recursive_mutex: "terminating due to uncaught exception ... recursive_mutex lock failed",
        // once per worker, after the last line of output. Constructing it here puts Core earlier in the
        // order than the pool, so it outlives every worker by construction.
        //
        // FOUND BY WIDENING WHO USES THE POOL (Г10). It was already latent: the JobSystem suite's main()
        // has always called Shutdown() explicitly "to join workers before static destruction", which is
        // this defect written down as a workaround. Six cloud suites that had never touched the pool
        // started a bake on it and crashed at exit with all their tests passed — the exact shape §1.4
        // warns about, a run that reports success and then dies.
        //
        // The workaround is only needed while Optick exists in the binary at all: with the profiler cut
        // out there is no `Optick::Core` to be destroyed early, so there is nothing to order against.
#if USE_OPTICK && DESERT_DEV_INSTRUMENTS
        (void)::Optick::IsActive();
#endif

        // cores - 2: the game thread and the render thread keep a core each (the render thread records the
        // graph's serial passes and replays the chunks the workers recorded; a worker on its core only delays
        // that). At least one worker so Submit() always makes progress on a one- or two-core machine.
        const unsigned hw      = std::thread::hardware_concurrency();
        const size_t   workers = hw > 3 ? hw - 2 : 1u;
        // Background jobs on cores - 3 at most (one worker fewer), never fewer than one: a one-worker pool
        // runs its Background jobs too, between the others.
        m_BackgroundCap = workers > 1 ? workers - 1 : 1u;
        m_Workers.reserve( workers );
        for ( size_t i = 0; i < workers; ++i )
            m_Workers.emplace_back( [this] { WorkerLoop(); } );
    }

    JobSystem::~JobSystem()
    {
        Shutdown();
    }

    void JobSystem::WorkerLoop()
    {
        // Held for the worker's whole life: without it a stack overflow in a job dies with no crash
        // report on POSIX (CR1d; see ThreadCrashStackScope).
        const Crash::ThreadCrashStackScope crashStack;
        // Register with Optick so job scopes (e.g. parallel ECS systems) show on their own timeline
        // rows instead of being silently dropped for an unknown thread.
#if DESERT_DEV_INSTRUMENTS
        OPTICK_THREAD( "JobSystem Worker" );
#endif
        for ( ;; )
        {
            QueuedJob queued;
            bool      background = false;
            {
                std::unique_lock<std::mutex> lk( m_Mutex );
                // A Background job past the cap is not taken, so the wait ends on a queue this worker may take
                // from; at Stop every queue drains, the cap aside (nothing else is left to run then).
                size_t from = kPriorityCount;
                m_CV.wait( lk,
                           [this, &from]
                           {
                               from = NextQueueLocked();
                               return from != kPriorityCount || m_Stop;
                           } );
                if ( from == kPriorityCount )
                {
                    const auto backgroundQueue = static_cast<size_t>( JobPriority::Background );
                    if ( m_Queues[backgroundQueue].empty() )
                        return; // stopping, every queue drained
                    from = backgroundQueue;
                }
                queued = std::move( m_Queues[from].front() );
                m_Queues[from].pop_front();
                ++m_Running;
                background = from == static_cast<size_t>( JobPriority::Background );
                if ( background )
                    ++m_RunningBackground;
            }
            InlineJob& job = queued.Job;
            // THE SUBMITTER'S FRAME for the job's run (RecordingFrame): a worker is unbound between jobs.
            RecordingFrame& binding = RecordingFrame::OfThisThread();
            binding                 = queued.Recording;

            // Never let a throwing job escape the worker thread: an uncaught exception here unwinds
            // through this loop (and Optick's per-thread ThreadScope) and calls std::terminate(), taking
            // the WHOLE app down — which showed up as a hard crash mid-startup while a background asset
            // preload threw. Swallow + log so one bad job degrades gracefully and names itself.
            try
            {
                job();
            }
            catch ( const std::exception& e )
            {
                LOG_ERROR( "[JobSystem] Worker job threw '{}' — swallowed to keep the app alive.", e.what() );
            }
            catch ( ... )
            {
                LOG_ERROR( "[JobSystem] Worker job threw a non-std exception — swallowed to keep the app alive." );
            }

            binding = RecordingFrame{};
            // The job's captures die here, unbound: a deletion they queue takes the game thread's frame, as on
            // the render task after a job.
            queued = QueuedJob{};

            bool backgroundWaits = false;
            {
                std::lock_guard<std::mutex> lk( m_Mutex );
                --m_Running;
                if ( background )
                {
                    --m_RunningBackground;
                    backgroundWaits = !m_Queues[static_cast<size_t>( JobPriority::Background )].empty();
                }
            }
            // A Background slot came free: a worker parked on the cap may take the next one. This worker loops
            // back and may take it itself; the wake covers a queue it leaves for a High job that came first.
            if ( backgroundWaits )
                m_CV.notify_one();
        }
    }

    size_t JobSystem::NextQueueLocked() const
    {
        for ( size_t p = 0; p < kPriorityCount; ++p )
        {
            if ( m_Queues[p].empty() )
                continue;
            if ( p == static_cast<size_t>( JobPriority::Background ) && m_RunningBackground >= m_BackgroundCap )
                return kPriorityCount;
            return p;
        }
        return kPriorityCount;
    }

    bool JobSystem::Submit( InlineJob job, const JobPriority priority )
    {
        {
            std::lock_guard<std::mutex> lk( m_Mutex );
            if ( m_Stop )
                return false; // shutting down: dropped, and the caller is told so
            const size_t queue = std::min( static_cast<size_t>( priority ), kPriorityCount - 1u );
            m_Queues[queue].push_back( QueuedJob{ std::move( job ), RecordingFrame::OfThisThread() } );
        }
        m_CV.notify_one();
        return true;
    }

    size_t JobSystem::PendingJobs() const
    {
        std::lock_guard<std::mutex> lk( m_Mutex );
        size_t                      queued = 0;
        for ( const std::deque<QueuedJob>& queue : m_Queues )
            queued += queue.size();
        return queued + m_Running;
    }

    namespace
    {
        /// The state one ParallelRanges call shares with its helpers. Held by shared_ptr, so a helper the
        /// pool never got round to scheduling can still run — long after the call returned — and find an
        /// empty cursor rather than a dead object.
        ///
        /// THE CLAIM AND THE ACTIVE COUNT MOVE TOGETHER, UNDER ONE LOCK, and that is the whole safety
        /// argument. A lock-free cursor would let a helper take a range and then be suspended before it
        /// announced itself; the caller would see an empty cursor and no active helper, return, and the
        /// helper would wake up and run the body against a destroyed stack frame. Handing out a range and
        /// counting it are one indivisible step here, so "the cursor is empty and nobody is active" really
        /// does mean nobody will ever touch the caller's frame again.
        struct RangeShare
        {
            std::mutex              Mutex;
            std::condition_variable Done;
            size_t                  Next   = 0; // next range to hand out
            size_t                  Count  = 0; // how many ranges there are
            size_t                  Active = 0; // ranges a HELPER is executing right now

            /// @return false when there is nothing left; @p helper says whether to count the claim.
            bool Claim( size_t& out, bool helper )
            {
                std::lock_guard<std::mutex> lk( Mutex );
                if ( Next == Count )
                    return false;
                out = Next++;
                if ( helper )
                    ++Active;
                return true;
            }

            void Finish()
            {
                std::lock_guard<std::mutex> lk( Mutex );
                if ( --Active == 0 )
                    Done.notify_one();
            }

            void WaitForHelpers()
            {
                std::unique_lock<std::mutex> lk( Mutex );
                Done.wait( lk, [this] { return Active == 0; } );
            }
        };
    } // namespace

    void JobSystem::ParallelRanges( size_t count, size_t grain,
                                    const std::function<void( size_t begin, size_t end )>& body,
                                    const JobPriority                                      priority )
    {
        if ( count == 0 )
            return;
        if ( grain == 0 )
            grain = 1;

        const size_t ranges = ( count + grain - 1 ) / grain;
        if ( ranges == 1 || m_Workers.empty() )
        {
            body( 0, count );
            return;
        }

        auto share   = std::make_shared<RangeShare>();
        share->Count = ranges;

        // One helper per worker at most, and never more than there are ranges left once the caller has
        // taken one: a helper that can only ever find an empty cursor is a queue slot spent on nothing.
        const size_t helpers = std::min( ranges - 1, m_Workers.size() );

        // `&body` and not a copy: the closure fits InlineJob's 64-byte inline storage this way (shared_ptr
        // + pointer + two sizes), so a parallel loop costs no heap allocation per helper. Dereferencing it
        // is only ever reached through a SUCCESSFUL claim, and a successful claim cannot happen after the
        // caller has returned — see RangeShare.
        const std::function<void( size_t, size_t )>* bodyPtr = &body;

        for ( size_t h = 0; h < helpers; ++h )
        {
            Submit(
                 [share, bodyPtr, count, grain]
                 {
                     size_t index = 0;
                     while ( share->Claim( index, true ) )
                     {
                         // Finish() must run even if the body throws, or the caller waits for a helper
                         // that is already gone. The worker loop logs and swallows the exception above us.
                         struct Guard
                         {
                             RangeShare* Share;
                             ~Guard()
                             {
                                 Share->Finish();
                             }
                         } guard{ share.get() };

                         const size_t begin = index * grain;
                         ( *bodyPtr )( begin, std::min( count, begin + grain ) );
                     }
                 },
                 priority );
        }

        // THE CALLER WORKS TOO, and it is what makes the loop independent of the pool: if every worker is
        // busy the cursor still empties, here, serially.
        try
        {
            size_t index = 0;
            while ( share->Claim( index, false ) )
            {
                const size_t begin = index * grain;
                body( begin, std::min( count, begin + grain ) );
            }
        }
        catch ( ... )
        {
            // A throwing body still leaves helpers running against this stack frame. Drain them before
            // letting the exception out, or the frame dies underneath them.
            share->WaitForHelpers();
            throw;
        }

        share->WaitForHelpers();
    }

    void JobSystem::ParallelFor( size_t count, const std::function<void( size_t )>& body,
                                 const JobPriority priority )
    {
        // About four ranges per participant. One range per participant is what the old implementation did
        // and it is only right when every index costs the same; four lets a participant that drew the
        // cheap end come back for more. More than that starts paying the claim's lock for nothing.
        const size_t participants = m_Workers.size() + 1;
        const size_t grain        = std::max<size_t>( 1, count / ( participants * 4 ) );

        ParallelRanges(
             count, grain,
             [&body]( size_t begin, size_t end )
             {
                 for ( size_t i = begin; i < end; ++i )
                     body( i );
             },
             priority );
    }

    void JobSystem::Shutdown()
    {
        {
            std::lock_guard<std::mutex> lk( m_Mutex );
            if ( m_Stop )
                return;
            m_Stop = true;
        }
        m_CV.notify_all();
        for ( auto& worker : m_Workers )
            if ( worker.joinable() )
                worker.join();
        m_Workers.clear();
    }
} // namespace Common
