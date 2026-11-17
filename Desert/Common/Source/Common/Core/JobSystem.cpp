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
#include <chrono>
#include <format>
#include <memory>
#include <string>

#if defined( _M_X64 ) || defined( _M_IX86 ) || defined( __x86_64__ ) || defined( __i386__ )
#include <immintrin.h> // _mm_pause: the record-phase spin (SpinForHighWork)
#elif defined( _M_ARM64 )
#include <intrin.h> // __yield
#endif

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

    namespace
    {
        // THE RECORD PHASE'S SPIN (UE LowLevelTasks FWaitingQueue: spin, then park). A worker that just ran a High
        // job - the frame's record chunks and setup tasks are the only High work (JobPriority) - pause-spins this
        // long on the queued-work hint before it parks, so the next chunk of the same burst is taken without an
        // OS wake (a condition-variable wake costs 5-30 us on Windows, as long as a small chunk). Bounded and
        // gated: after a Normal or Background job a worker parks at once, and the spin never outlasts this.
        constexpr std::chrono::microseconds kHighSpinMicros{ 30 };
        // Pauses between two clock reads: ~1-2 us of spin on x64, so the clock is not the spin's cost.
        constexpr int kPausesPerClockRead = 32;

        inline void CpuRelax() noexcept
        {
#if defined( _M_X64 ) || defined( _M_IX86 ) || defined( __x86_64__ ) || defined( __i386__ )
            _mm_pause();
#elif defined( _M_ARM64 )
            __yield();
#elif defined( __aarch64__ ) || defined( __arm__ )
            __asm__ __volatile__( "yield" );
#else
            std::this_thread::yield();
#endif
        }

        constexpr size_t kBackgroundQueue = static_cast<size_t>( JobPriority::Background );
        constexpr size_t kHighQueue       = static_cast<size_t>( JobPriority::High );
        // The first ring a priority allocates; it doubles from there and never shrinks.
        constexpr size_t kRingInitialCapacity = 64;
    } // namespace

    void JobSystem::JobRing::Reserve( const size_t extra )
    {
        const size_t size     = Size();
        const size_t capacity = m_Slots ? m_Mask + 1 : 0;
        if ( size + extra <= capacity )
            return;
        size_t grown = capacity > 0 ? capacity : kRingInitialCapacity;
        while ( grown < size + extra )
            grown *= 2;
        std::unique_ptr<QueuedJob[]> slots( new QueuedJob[grown] );
        for ( size_t i = 0; i < size; ++i )
            slots[i] = std::move( m_Slots[( m_Head + i ) & m_Mask] ); // FIFO order kept across the grow
        m_Slots = std::move( slots );
        m_Mask  = grown - 1;
        m_Head  = 0;
        m_Tail  = size;
    }

    void JobSystem::JobRing::PushBack( QueuedJob&& job )
    {
        Reserve( 1 );
        m_Slots[m_Tail & m_Mask] = std::move( job );
        ++m_Tail;
    }

    JobSystem::QueuedJob JobSystem::JobRing::PopFront() noexcept
    {
        // The slot keeps a moved-from (empty) job until a push overwrites it: no captures linger in it.
        QueuedJob job = std::move( m_Slots[m_Head & m_Mask] );
        ++m_Head;
        return job;
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
        bool afterHigh = false;
        for ( ;; )
        {
            if ( afterHigh )
                SpinForHighWork();
            QueuedJob queued;
            bool      background = false;
            {
                std::unique_lock<std::mutex> lk( m_Mutex );
                // A Background job past the cap is not taken, so the wait ends on a queue this worker may take
                // from; at Stop every queue drains, the cap aside (nothing else is left to run then). The worker
                // counts itself parked (m_Sleeping) around the wait, under the lock a submitter reads it under,
                // so a submit that sees nobody parked skips the notify and still cannot be missed: this worker
                // reads the queues under that lock before it parks.
                size_t from = NextQueueLocked();
                while ( from == kPriorityCount && !m_Stop )
                {
                    ++m_Sleeping;
                    m_CV.wait( lk );
                    --m_Sleeping;
                    from = NextQueueLocked();
                }
                if ( from == kPriorityCount )
                {
                    if ( m_Queues[kBackgroundQueue].Empty() )
                        return; // stopping, every queue drained
                    from = kBackgroundQueue;
                }
                queued = m_Queues[from].PopFront();
                m_Running.fetch_add( 1, std::memory_order_relaxed );
                background = from == kBackgroundQueue;
                if ( background )
                    m_RunningBackground.fetch_add( 1, std::memory_order_relaxed );
                else
                    m_QueuedHint.fetch_sub( 1, std::memory_order_relaxed );
                afterHigh = from == kHighQueue;
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

            // A High or Normal job ends WITHOUT THE LOCK: nothing waits on m_Running (PendingJobs only reads it).
            m_Running.fetch_sub( 1, std::memory_order_relaxed );
            if ( !background )
                continue;
            // A Background slot came free: the cap is read under the lock, so the slot is returned under it, and a
            // worker parked on the cap may take the next one. This worker loops back and may take it itself; the
            // wake covers a queue it leaves for a High job that came first.
            bool backgroundWaits = false;
            {
                std::lock_guard<std::mutex> lk( m_Mutex );
                m_RunningBackground.fetch_sub( 1, std::memory_order_relaxed );
                backgroundWaits = !m_Queues[kBackgroundQueue].Empty() && m_Sleeping > 0;
            }
            if ( backgroundWaits )
                m_CV.notify_one();
        }
    }

    void JobSystem::SpinForHighWork() const noexcept
    {
        const auto start = std::chrono::steady_clock::now();
        for ( ;; )
        {
            for ( int i = 0; i < kPausesPerClockRead; ++i )
            {
                if ( m_QueuedHint.load( std::memory_order_relaxed ) != 0 )
                    return;
                CpuRelax();
            }
            if ( std::chrono::steady_clock::now() - start >= kHighSpinMicros )
                return;
        }
    }

    size_t JobSystem::NextQueueLocked() const
    {
        for ( size_t p = 0; p < kPriorityCount; ++p )
        {
            if ( m_Queues[p].Empty() )
                continue;
            if ( p == kBackgroundQueue &&
                 m_RunningBackground.load( std::memory_order_relaxed ) >= m_BackgroundCap )
                return kPriorityCount;
            return p;
        }
        return kPriorityCount;
    }

    size_t JobSystem::WakesForLocked( const size_t jobs ) const noexcept
    {
        return std::min( jobs, m_Sleeping );
    }

    void JobSystem::Wake( const size_t wakes ) noexcept
    {
        if ( wakes == 0 )
            return;
        if ( wakes >= m_Workers.size() )
        {
            m_CV.notify_all();
            return;
        }
        for ( size_t w = 0; w < wakes; ++w )
            m_CV.notify_one();
    }

    bool JobSystem::Submit( InlineJob job, const JobPriority priority )
    {
        size_t wakes = 0;
        {
            std::lock_guard<std::mutex> lk( m_Mutex );
            if ( m_Stop )
                return false; // shutting down: dropped, and the caller is told so
            const size_t queue = std::min( static_cast<size_t>( priority ), kPriorityCount - 1u );
            m_Queues[queue].PushBack( QueuedJob{ std::move( job ), RecordingFrame::OfThisThread() } );
            if ( queue != kBackgroundQueue )
                m_QueuedHint.fetch_add( 1, std::memory_order_relaxed );
            wakes = WakesForLocked( 1 );
        }
        Wake( wakes );
        return true;
    }

    bool JobSystem::SubmitBatch( const std::span<InlineJob> jobs, const JobPriority priority )
    {
        if ( jobs.empty() )
            return true;
        const RecordingFrame recording = RecordingFrame::OfThisThread();
        size_t               wakes     = 0;
        {
            std::lock_guard<std::mutex> lk( m_Mutex );
            if ( m_Stop )
                return false; // shutting down: no job taken, the caller still owns every one
            const size_t queue = std::min( static_cast<size_t>( priority ), kPriorityCount - 1u );
            JobRing&     ring  = m_Queues[queue];
            ring.Reserve( jobs.size() );
            for ( InlineJob& job : jobs )
                ring.PushBack( QueuedJob{ std::move( job ), recording } );
            if ( queue != kBackgroundQueue )
                m_QueuedHint.fetch_add( jobs.size(), std::memory_order_relaxed );
            wakes = WakesForLocked( jobs.size() );
        }
        Wake( wakes );
        return true;
    }

    size_t JobSystem::PendingJobs() const
    {
        std::lock_guard<std::mutex> lk( m_Mutex );
        size_t                      queued = 0;
        for ( const JobRing& queue : m_Queues )
            queued += queue.Size();
        return queued + m_Running.load( std::memory_order_relaxed );
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

        // The helpers go to the pool in batches under one lock each (SubmitBatch), not one lock and one wake per
        // helper. A refused batch (shutting down) owes nothing: the caller below claims every range itself.
        constexpr size_t kHelperBatch = 64;
        InlineJob        batch[kHelperBatch];
        size_t           filled = 0;
        for ( size_t h = 0; h < helpers; ++h )
        {
            batch[filled++] = InlineJob(
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
                 } );
            if ( filled == kHelperBatch || h + 1 == helpers )
            {
                (void)SubmitBatch( std::span<InlineJob>( batch, filled ), priority );
                filled = 0;
            }
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
            const char* Name = nullptr;
            InlineJob   Body; // noexcept by type, inline up to InlineJob::kInlineSize: no heap per task
            bool        Claimed = false;
        };

        std::mutex              Mutex;
        std::condition_variable Done;
        std::vector<Task>       Tasks;
        size_t                  Active = 0; // tasks a WORKER runs right now

        /// Under the lock: task @p index, unless someone has it. @p helper counts the claim as active.
        bool Claim( const size_t index, InlineJob& body, const char*& name, const bool helper )
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
        void Run( const char* name, InlineJob& body ) noexcept
        {
#if DESERT_DEV_INSTRUMENTS
            OPTICK_EVENT_DYNAMIC( name );
#else
            (void)name;
#endif
            ++t_TaskDepth;
            body();
            --t_TaskDepth;
            body = InlineJob{}; // the task's captures die on the thread that ran it, before the join returns
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
        // A group of one task per participant (the common shape, LaunchSlices) appends without a regrow.
        if ( m_Parallel )
            m_Shared->Tasks.reserve( JobSystem::Get().WorkerCount() + 1 );
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

    size_t TaskGroup::SliceCount( const size_t count, const size_t minBatch ) noexcept
    {
        const size_t floor        = std::max<size_t>( 1, minBatch );
        const size_t participants = JobSystem::Get().WorkerCount() + 1;
        return std::clamp<size_t>( count / floor, 1, participants );
    }

    void TaskGroup::RunHere( const char* name, InlineJob& task ) noexcept
    {
        m_Shared->Run( name, task );
    }

    void TaskGroup::Launch( const char* name, InlineJob task )
    {
        m_Joined = false;
        if ( !m_Parallel )
        {
            // The switch off: the task runs here and now, still under InTask (the builder's check holds either
            // way).
            m_Shared->Run( name, task );
            return;
        }
        LaunchBatch( name, std::span<InlineJob>( &task, 1 ) );
    }

    void TaskGroup::LaunchBatch( const char* name, const std::span<InlineJob> tasks )
    {
        m_Joined    = false;
        size_t base = 0;
        {
            std::lock_guard<std::mutex> lk( m_Shared->Mutex );
            base = m_Shared->Tasks.size();
            for ( InlineJob& task : tasks )
                m_Shared->Tasks.push_back( Shared::Task{ name, std::move( task ), false } );
        }
        // The span's slots are empty now (moved into the group); they carry the claim jobs to the pool, in launch
        // order, under one lock (SubmitBatch). A batch the pool refuses (shutting down) owes nothing: the tasks
        // stay unclaimed and Join runs them.
        for ( size_t i = 0; i < tasks.size(); ++i )
        {
            std::shared_ptr<Shared> shared = m_Shared;
            const size_t            index  = base + i;
            tasks[i]                       = InlineJob(
                 [shared, index]() noexcept
                 {
                     InlineJob   body;
                     const char* taskName = nullptr;
                     if ( !shared->Claim( index, body, taskName, true ) )
                         return;
                     shared->Run( taskName, body );
                     shared->Finish();
                 } );
        }
        (void)JobSystem::Get().SubmitBatch( tasks, JobPriority::High );
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
            InlineJob   body;
            const char* name = nullptr;
            if ( m_Shared->Claim( index, body, name, false ) )
                m_Shared->Run( name, body );
        }
        std::unique_lock<std::mutex> lk( m_Shared->Mutex );
        m_Shared->Done.wait( lk, [this] { return m_Shared->Active == 0; } );
    }
} // namespace Common
