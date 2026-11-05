#include "JobSystem.hpp"

#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/EngineThread.hpp>

// Optick is on the shipping link line and contributes nothing to it, because after the two guards below
// no translation unit in this engine references a single Optick symbol — the profiling macros are empty
// (Common/Core/Profiler.hpp) and these are the only other two uses in the tree. That absence is not
// asserted by reading the build scripts; scripts/CI/ShippingSymbols.sh reads the linked binary.
#include <optick.h>

#include <algorithm>
#include <format>
#include <memory>
#include <string>

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
            m_Workers.emplace_back( [this, i]() noexcept { WorkerLoop( i ); } );
    }

    JobSystem::~JobSystem() noexcept
    {
        Shutdown();
    }

    void JobSystem::WorkerLoop( const size_t index ) noexcept
    {
        // Held for the worker's whole life: without it a stack overflow in a job dies with no crash
        // report on POSIX (CR1d; see ThreadCrashStackScope).
        const Crash::ThreadCrashStackScope crashStack;
        // Named for debuggers, captures and the '[Threads]' census, which counts "Job Worker #i" as one role;
        // Normal: the frame's two AboveNormal threads keep a core each and the workers have the rest
        // (workers = cores - 2). Once per worker, at its start: the name's string is not on a frame's path.
        NameCurrentThread( std::format( "Job Worker #{}", index ), ThreadPriority::Normal );
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

            // NOTHING IS CAUGHT HERE. InlineJob::operator() is noexcept, so a job that throws ends the process at
            // the job, and EntryPoint's terminate handler prints what() and the stack. Catching it here would
            // leave the job's tail unrun (its counter, its hand-over) and the failure would surface far away.
            job();

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

        // EVERY RANGE RUNS INSIDE THIS noexcept FUNCTION, on the caller and on each helper. A body that throws
        // ends the process at the range that threw (std::terminate, EntryPoint's handler). So no helper is left
        // running against a caller frame that unwound, and no throw reaches a worker loop one level up.
        void RunRange( const std::function<void( size_t, size_t )>& body, size_t begin, size_t end ) noexcept
        {
            body( begin, end );
        }
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
            RunRange( body, 0, count );
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
                 [share, bodyPtr, count, grain]() noexcept
                 {
                     size_t index = 0;
                     while ( share->Claim( index, true ) )
                     {
                         // RunRange returns or ends the process, so Finish() always follows a claimed range.
                         const size_t begin = index * grain;
                         RunRange( *bodyPtr, begin, std::min( count, begin + grain ) );
                         share->Finish();
                     }
                 },
                 priority );
        }

        // THE CALLER WORKS TOO, and it is what makes the loop independent of the pool: if every worker is
        // busy the cursor still empties, here, serially.
        size_t index = 0;
        while ( share->Claim( index, false ) )
        {
            const size_t begin = index * grain;
            RunRange( body, begin, std::min( count, begin + grain ) );
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

    void JobSystem::Shutdown() noexcept
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

    namespace
    {
        // How many TaskGroup tasks this thread is inside (a task may run a group of its own).
        thread_local uint32_t t_TaskDepth = 0;
    } // namespace

    /// The tasks one TaskGroup shares with the jobs it submitted. Held by shared_ptr, so a job the pool runs after
    /// the group was joined and destroyed finds every task claimed rather than a dead object. A task is CLAIMED
    /// ONCE, UNDER THE LOCK, together with the count of tasks a worker runs: "every task claimed and nothing
    /// active" then really means no thread will touch the launcher's frame again (RangeShare's argument).
    struct TaskGroup::Shared
    {
        struct Task
        {
            const char*           Name = nullptr;
            std::function<void()> Body;
            bool                  Claimed = false;
        };

        std::mutex              Mutex;
        std::condition_variable Done;
        std::vector<Task>       Tasks;
        size_t                  Active = 0; // tasks a WORKER runs right now

        /// Under the lock: task @p index, unless someone has it. @p helper counts the claim as active.
        bool Claim( const size_t index, std::function<void()>& body, const char*& name, const bool helper )
        {
            std::lock_guard<std::mutex> lk( Mutex );
            if ( index >= Tasks.size() || Tasks[index].Claimed )
                return false;
            Tasks[index].Claimed = true;
            body                 = std::move( Tasks[index].Body );
            name                 = Tasks[index].Name;
            if ( helper )
                ++Active;
            return true;
        }

        /// A task body is noexcept by contract: no catch here, a throw terminates at its own site.
        void Run( const char* name, std::function<void()>& body ) noexcept
        {
#if DESERT_DEV_INSTRUMENTS
            OPTICK_EVENT_DYNAMIC( name );
#else
            (void)name;
#endif
            ++t_TaskDepth;
            body();
            --t_TaskDepth;
            body = nullptr; // the task's captures die on the thread that ran it, before the join returns
        }

        void Finish()
        {
            std::lock_guard<std::mutex> lk( Mutex );
            if ( --Active == 0 )
                Done.notify_all();
        }
    };

    TaskGroup::TaskGroup( const bool parallel )
         : m_Shared( std::make_shared<Shared>() ), m_Parallel( parallel && JobSystem::Get().WorkerCount() > 0 )
    {
    }

    TaskGroup::~TaskGroup()
    {
        if ( !m_Joined )
            Join();
    }

    bool TaskGroup::InTask() noexcept
    {
        return t_TaskDepth > 0;
    }

    void TaskGroup::Launch( const char* name, std::function<void()> task )
    {
        m_Joined = false;
        if ( !m_Parallel )
        {
            // The switch off: the task runs here and now, still under InTask (the builder's check holds either
            // way).
            m_Shared->Run( name, task );
            return;
        }
        size_t index = 0;
        {
            std::lock_guard<std::mutex> lk( m_Shared->Mutex );
            index = m_Shared->Tasks.size();
            m_Shared->Tasks.push_back( Shared::Task{ name, std::move( task ), false } );
        }
        // A job the pool refuses (shutting down) owes nothing: the task stays unclaimed and Join runs it.
        std::shared_ptr<Shared> shared = m_Shared;
        JobSystem::Get().Submit(
             [shared, index]() noexcept
             {
                 std::function<void()> body;
                 const char*           taskName = nullptr;
                 if ( !shared->Claim( index, body, taskName, true ) )
                     return;
                 shared->Run( taskName, body );
                 shared->Finish();
             },
             JobPriority::High );
    }

    void TaskGroup::Join() noexcept
    {
        m_Joined = true;
        // THE CALLER WORKS FIRST: every task no worker has claimed runs here, so the wait below is only ever for a
        // task that is running on a worker - never for a job still in a queue.
        for ( size_t index = 0;; ++index )
        {
            {
                std::lock_guard<std::mutex> lk( m_Shared->Mutex );
                if ( index >= m_Shared->Tasks.size() )
                    break;
            }
            std::function<void()> body;
            const char*           name = nullptr;
            if ( m_Shared->Claim( index, body, name, false ) )
                m_Shared->Run( name, body );
        }
        std::unique_lock<std::mutex> lk( m_Shared->Mutex );
        m_Shared->Done.wait( lk, [this] { return m_Shared->Active == 0; } );
    }
} // namespace Common
