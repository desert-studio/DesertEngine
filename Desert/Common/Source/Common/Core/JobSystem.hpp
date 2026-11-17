#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace Common
{
    // Small-buffer callable for the job queue: closures up to kInlineSize bytes live INLINE in the
    // queue node — zero heap per job (std::function's SBO is too small for a typical ParallelFor
    // chunk, so every submit paid a heap alloc + free). Larger captures fall back to one heap
    // block; that stays correct, just the exception rather than the rule.
    class InlineJob
    {
    public:
        static constexpr std::size_t kInlineSize = 64;

        InlineJob() = default;

        template <typename F, typename D = std::decay_t<F>,
                  typename = std::enable_if_t<!std::is_same_v<D, InlineJob>>>
        InlineJob( F&& fn )
        {
            // A JOB IS noexcept BY TYPE (UE: engine code does not catch). A callable that can fail returns its
            // error as a value (Common::ResultStr) and says noexcept; a job not declared noexcept does not compile.
            static_assert( std::is_nothrow_invocable_v<D&>,
                           "a job is noexcept: declare the lambda noexcept and return its failure as a value" );
            if constexpr ( sizeof( D ) <= kInlineSize && alignof( D ) <= alignof( std::max_align_t ) &&
                           std::is_nothrow_move_constructible_v<D> )
            {
                new ( m_Storage ) D( std::forward<F>( fn ) );
                m_Ops = &s_InlineOps<D>;
            }
            else
            {
                *reinterpret_cast<D**>( static_cast<void*>( m_Storage ) ) = new D( std::forward<F>( fn ) );
                m_Ops = &s_HeapOps<D>;
            }
        }

        InlineJob( InlineJob&& other ) noexcept
        {
            MoveFrom( other );
        }
        InlineJob& operator=( InlineJob&& other ) noexcept
        {
            if ( this != &other )
            {
                Destroy();
                MoveFrom( other );
            }
            return *this;
        }
        InlineJob( const InlineJob& )            = delete;
        InlineJob& operator=( const InlineJob& ) = delete;
        ~InlineJob()
        {
            Destroy();
        }

        // THE JOB RUNS INSIDE A noexcept FUNCTION (UE: engine code does not catch). A throw from the callable
        // ends the process at the job, through std::terminate and EntryPoint's handler, which prints what()
        // and the stack. It never unwinds into the worker loop, which could only swallow it or die later.
        // A job that can fail returns a value; a ThirdParty call that throws is wrapped once at its boundary.
        void operator()() noexcept
        {
            m_Ops->Invoke( m_Storage );
        }
        explicit operator bool() const
        {
            return m_Ops != nullptr;
        }

    private:
        struct Ops
        {
            void ( *Invoke )( void* ) noexcept;
            void ( *MoveTo )( void* src, void* dst ); // dst is uninitialized; src is destroyed
            void ( *Destroy )( void* );
        };

        // std::destroy_at rather than an explicit `->~D()`: D is a lambda closure type here, and MSVC
        // rejects the destructor-call syntax on a template parameter naming a closure ("class has no
        // destructor called '~D'"). destroy_at is equivalent, standard, and accepted by both compilers.
        template <typename D>
        static constexpr Ops s_InlineOps = { []( void* s ) noexcept { ( *static_cast<D*>( s ) )(); },
                                             []( void* src, void* dst )
                                             {
                                                 new ( dst ) D( std::move( *static_cast<D*>( src ) ) );
                                                 std::destroy_at( static_cast<D*>( src ) );
                                             },
                                             []( void* s ) { std::destroy_at( static_cast<D*>( s ) ); } };

        template <typename D>
        static constexpr Ops s_HeapOps = { []( void* s ) noexcept { ( **static_cast<D**>( s ) )(); },
                                           []( void* src, void* dst )
                                           { *static_cast<D**>( dst ) = *static_cast<D**>( src ); },
                                           []( void* s ) { delete *static_cast<D**>( s ); } };

        void MoveFrom( InlineJob& other ) noexcept
        {
            m_Ops = other.m_Ops;
            if ( m_Ops )
                m_Ops->MoveTo( other.m_Storage, m_Storage );
            other.m_Ops = nullptr;
        }
        void Destroy()
        {
            if ( m_Ops )
            {
                m_Ops->Destroy( m_Storage );
                m_Ops = nullptr;
            }
        }

        alignas( std::max_align_t ) unsigned char m_Storage[kInlineSize];
        const Ops* m_Ops = nullptr;
    };

    // THE FRAME A THREAD RECORDS FOR, CARRIED FROM THE THREAD THAT SUBMITS A JOB TO THE WORKER THAT RUNS IT
    // (OVERLAP-ON-7; UE: a task the render thread spawns reads the render frame, GFrameNumberRenderThread,
    // wherever it runs). Engine::FrameManager owns the meaning: ScopedRecordingFrame binds it on the render task
    // around one job, and GetCurrentFrameIndex / GetAbsoluteFrameCount (the RENDER frame) answer it while bound,
    // else the game thread's frame. JobSystem::Submit captures the submitter's binding and the worker runs the job
    // under it, so a pass the render graph records on a worker keeps its job's slot and frame number after the
    // game thread has counted the next frame (a material prepared on the render task and checked on a worker saw
    // two numbers and was dropped; a uniform buffer bound the game thread's slot). A job a game-side thread
    // submits runs unbound: it reads the game thread's frame, as its submitter does.
    struct RecordingFrame
    {
        static constexpr uint32_t k_Unbound = 0xFFFFFFFFu;
        uint32_t                  Slot      = k_Unbound; // the frame-in-flight slot; k_Unbound: no binding
        uint64_t                  Frame     = 0;         // the absolute frame number, read only while bound

        [[nodiscard]] bool Bound() const noexcept
        {
            return Slot != k_Unbound;
        }
        /// The calling thread's binding (one per thread; Engine::FrameManager::ScopedRecordingFrame writes it).
        [[nodiscard]] static RecordingFrame& OfThisThread() noexcept
        {
            static thread_local RecordingFrame t_Binding;
            return t_Binding;
        }
    };

    // WHO A JOB SERVES, AND SO WHICH ONE A FREE WORKER TAKES FIRST (UE ENamedThreads / LowLevelTasks
    // ETaskPriority: High for the frame's critical path, Normal, BackgroundNormal). A worker takes the oldest
    // High job, else the oldest Normal one, else the oldest Background one - and a Background job only while
    // fewer than JobSystem::BackgroundWorkerCap() Background jobs run, so a thumbnail bake, an import or a
    // distance-field build can never hold every worker when the render thread hands out its record chunks.
    // A running job is never pre-empted: a High job waits at most for the shortest job a worker is running.
    enum class JobPriority : uint8_t
    {
        High,       // the frame: the render graph's record chunks, a view's setup work the builder thread joins
        Normal,     // the default: work a caller waits for soon (a parallel loop, a load the game thread awaits)
        Background, // work nobody waits for this frame (cooks, thumbnails, imports, bakes); capped below
        Count
    };

    // Engine-wide worker-thread pool — THE place for CPU-parallel work (asset cooking, LUT generation,
    // background mesh cooks, future parallel ECS). Replaces the ad-hoc one-off std::thread/std::async
    // sprinkled around the codebase so thread count stays bounded (workers = cores - 2: the game thread and
    // the render thread keep a core each, as UE sizes its workers) and work is observable in one system.
    //
    // Threading contract:
    //   - Submit()/Async() are safe from any thread; jobs may run on any worker in any order.
    //   - ParallelFor()/ParallelRanges() BLOCK until done and the CALLING thread works too. They are safe
    //     to call from the main thread AND FROM A WORKER — including from inside another one of them. See
    //     the note on ParallelRanges for why nesting used to hang and why it now cannot.
    //   - Jobs must not assume GPU/AssetManager/ECS access is safe — same rule the old ad-hoc threads had.
    class JobSystem
    {
    public:
        // Global pool, started lazily on first use; joined automatically at exit (or via Shutdown()).
        static JobSystem& Get();

        ~JobSystem() noexcept; // Shutdown()
        JobSystem( const JobSystem& )            = delete;
        JobSystem& operator=( const JobSystem& ) = delete;

        // Fire-and-forget. Closures <= InlineJob::kInlineSize bytes enqueue with ZERO heap allocations.
        // False when the pool is shutting down and the job was dropped unrun: a caller that counts its jobs in
        // flight (and waits for them) must take this one back.
        // @p priority: the queue it joins (JobPriority); a worker drains High, then Normal, then Background.
        bool Submit( InlineJob job, JobPriority priority = JobPriority::Normal );

        // N jobs under ONE lock, then min(N, parked workers) wakes (UE LowLevelTasks FWaitingQueue: a batch
        // wakes the sleepers it can feed, not one per job). The jobs keep their order in the queue (FIFO per
        // priority, as N Submit calls in a row). ALL OR NOTHING: true when every job was enqueued (each one is
        // moved from); false when the pool is shutting down - then no job was taken, every element of @p jobs
        // is still the caller's to run or drop, as one refused Submit.
        bool SubmitBatch( std::span<InlineJob> jobs, JobPriority priority = JobPriority::Normal );

        // Submit with a result: returns a std::future for the callable's return value. A promise and not a
        // packaged_task: packaged_task catches the callable's throw into the future, which a caller that never
        // calls get() never sees. The callable is noexcept by type (the static_assert below, as InlineJob's), so a
        // callable that can fail returns its error as the value. EVERY PROMISE IS KEPT: a job the pool refuses
        // (shutting down) runs here, on the caller, so get() returns the value and never throws broken_promise.
        template <typename F>
        auto Async( F&& fn, const JobPriority priority = JobPriority::Normal )
            -> std::future<std::invoke_result_t<std::decay_t<F>&>>
        {
            using D = std::decay_t<F>;
            using R = std::invoke_result_t<D&>;
            static_assert( std::is_nothrow_invocable_v<D&>,
                           "a job is noexcept: declare the lambda noexcept and return its failure as a value" );
            struct State
            {
                std::promise<R> Promise;
                D               Call;
            };
            auto       state = std::make_shared<State>( State{ std::promise<R>(), std::forward<F>( fn ) } );
            auto       fut   = state->Promise.get_future();
            const auto run   = [state]() noexcept
            {
                if constexpr ( std::is_void_v<R> )
                {
                    state->Call();
                    state->Promise.set_value();
                }
                else
                    state->Promise.set_value( state->Call() );
            };
            if ( !Submit( run, priority ) )
                run(); // the pool is shutting down: the job runs on the caller, so its promise is kept
            return fut;
        }

        /**
         * @brief Blocking parallel loop over [0, count) handed out in CONTIGUOUS RANGES of at most
         *        @p grain indices: body(begin, end) is called once per range and must process every index
         *        in [begin, end).
         *
         * WHY RANGES AND NOT INDICES. A body with per-range setup — a scratch buffer, a bin lookup, a
         * thread-local accumulator — cannot express itself through body(index) without either
         * reallocating per element or reaching for thread_local, and thread_local in a shared pool is
         * state that outlives the loop that filled it. ParallelFor below is this function with the range
         * walked for you, so there is ONE mechanism and not two.
         *
         * RANGES ARE CLAIMED, NOT DEALT. Every participant pulls the next unclaimed range off one cursor,
         * so a range that costs ten times its neighbours is absorbed by whoever finishes first instead of
         * holding the whole loop up. Pick @p grain so that one range is worth claiming (a lock and a
         * couple of hundred nanoseconds) and small enough that there are several per participant.
         *
         * SAFE TO CALL FROM A WORKER, INCLUDING FROM INSIDE ANOTHER PARALLEL LOOP, and that is the whole
         * reason this shape exists. The previous implementation dealt one chunk per participant up front
         * and then waited for EVERY chunk it had submitted. A chunk still sitting in the queue can only
         * ever run on a worker, so if every worker is itself blocked in such a wait, no chunk runs, no
         * worker is freed, and the pool hangs — a cycle, not a slowdown. The engine walked straight into
         * it: the cloud modelling bake runs ON a worker (VolumetricCloudRenderer), and parallelising its
         * z-slices with the old ParallelFor would have been exactly that nested call.
         *
         * WHY THE CYCLE CANNOT FORM NOW. This function never waits on a job that has not started:
         *   - all the work is claimed from one cursor, so a helper job that is never scheduled owes
         *     nothing and is not waited for;
         *   - the caller claims and runs ranges itself until the cursor is empty, so the loop always
         *     completes even if not one helper ever runs;
         *   - only then does it wait, and only on the ranges a helper has ALREADY CLAIMED — and a helper
         *     that has claimed one is by definition running on a worker thread.
         * The wait's precondition is therefore "a thread that is running will finish", never "a job needs
         * a free worker". Worst case — every worker busy — the caller does all the work serially, which
         * is precisely what the old implementation degraded to when it did not hang. Nesting is covered by
         * the same argument by induction: a body that calls this function again does not block on the pool
         * either, so "a running range finishes" stays true at any depth.
         *
         * WHAT IS STILL YOUR RESPONSIBILITY: the body must not wait for work only a WORKER can do — an
         * Async() future, a hand-rolled latch another job signals. That is the one way to reintroduce the
         * cycle, and no primitive here can detect it.
         *
         * @param grain 0 is read as 1.
         * @param priority the helpers' queue (JobPriority). The caller works whatever it is, so a Background
         *        loop still completes while every Background slot is taken - on the caller alone.
         */
        void ParallelRanges( size_t count, size_t grain,
                             const std::function<void( size_t begin, size_t end )>& body,
                             JobPriority priority = JobPriority::Normal );

        // Blocking parallel-for over [0, count): body(index). ParallelRanges with the range walked for
        // you and a grain of about a quarter of a participant's share, so the loop stays balanced when
        // the indices cost different amounts (importing files, running ECS systems — both do).
        void ParallelFor( size_t count, const std::function<void( size_t )>& body,
                          JobPriority priority = JobPriority::Normal );

        size_t WorkerCount() const
        {
            return m_Workers.size();
        }

        // How many Background jobs may run at once: cores - 3, at least one, and one fewer than the workers
        // whenever there are two or more, so a worker is always left for High and Normal work.
        size_t BackgroundWorkerCap() const
        {
            return m_BackgroundCap;
        }

        // Pending + currently-running job count (approximate; for tests/diagnostics).
        size_t PendingJobs() const;

        // Stops intake (Submit returns false from here on), lets the workers run every queued job - a Background
        // job past the cap too - and joins them (UE FTaskGraphInterface::Shutdown, Jolt StopThreads). A job that
        // submits while the pool drains is refused, and its owner runs it (TaskGroup::Join, ParallelRanges's
        // caller, Jolt's barrier). Idempotent; never from a worker, which would join itself.
        void Shutdown() noexcept;

    private:
        JobSystem();
        void WorkerLoop( size_t index ) noexcept; // named "Job Worker #index"

        // A queued job and the submitter's recording frame (RecordingFrame), bound around it on the worker.
        struct QueuedJob
        {
            InlineJob      Job;
            RecordingFrame Recording;
        };

        // ONE PRIORITY'S FIFO: a power-of-two ring of job slots (UE LowLevelTasks keeps its queues in fixed
        // arrays, not in node lists). std::deque allocated and freed a block every few dozen jobs at the frame's
        // submit rate; the ring grows by doubling when full and never shrinks, so after the first frames a push
        // and a pop touch no allocator. Only under JobSystem::m_Mutex.
        class JobRing
        {
        public:
            [[nodiscard]] bool Empty() const noexcept
            {
                return m_Head == m_Tail;
            }
            [[nodiscard]] size_t Size() const noexcept
            {
                return m_Tail - m_Head;
            }
            /// Room for @p extra more jobs without a grow inside the pushes (SubmitBatch: one grow, not log N).
            void Reserve( size_t extra );
            void PushBack( QueuedJob&& job );
            /// The oldest job, moved out; the ring must not be Empty().
            QueuedJob PopFront() noexcept;

        private:
            std::unique_ptr<QueuedJob[]> m_Slots;
            size_t                       m_Mask = 0; // capacity - 1; capacity 0 until the first push
            size_t                       m_Head = 0; // monotonic: the slot is m_Head & m_Mask
            size_t                       m_Tail = 0;
        };

        static constexpr size_t kPriorityCount = static_cast<size_t>( JobPriority::Count );
        static constexpr size_t kCacheLine     = 64;

        // Under m_Mutex: the queue a free worker takes from now, kPriorityCount when it may take none.
        size_t NextQueueLocked() const;
        // Under m_Mutex, the job pushed: wakes min(@p jobs, parked workers) after the caller unlocks.
        size_t WakesForLocked( size_t jobs ) const noexcept;
        void   Wake( size_t wakes ) noexcept;
        // After a High job (the frame's record phase, JobPriority): up to kHighSpinMicros of pause-spin on
        // m_QueuedHint before this worker parks, so the next record chunk is taken without an OS wake.
        void SpinForHighWork() const noexcept;

        // FALSE SHARING KEPT OFF THE HOT LINES (UE FWaitingQueue): the lock and its queue heads, the running
        // counters a finishing worker writes without the lock, and the lock-free hint a spinning worker reads
        // each sit on their own cache line.
        alignas( kCacheLine ) mutable std::mutex m_Mutex;
        std::condition_variable m_CV;
        JobRing                 m_Queues[kPriorityCount]; // indexed by JobPriority; under m_Mutex
        size_t                  m_Sleeping = 0;           // workers parked in m_CV.wait; under m_Mutex
        bool                    m_Stop     = false;       // under m_Mutex

        // Jobs executing / of them Background. Raised under m_Mutex when a job is taken (the Background cap is
        // checked there); lowered WITHOUT it when a job ends, so a job's completion takes no lock unless a
        // Background slot freed with Background work waiting.
        alignas( kCacheLine ) std::atomic<size_t> m_Running{ 0 };
        std::atomic<size_t> m_RunningBackground{ 0 };

        // High + Normal jobs queued: written under m_Mutex, read lock-free by SpinForHighWork.
        alignas( kCacheLine ) std::atomic<size_t> m_QueuedHint{ 0 };

        alignas( kCacheLine ) std::vector<std::thread> m_Workers;
        size_t m_BackgroundCap = 1; // BackgroundWorkerCap()
    };

    // A VIEW'S SETUP TASKS AND THE JOIN THAT ENDS THEM (RENDER-PARALLEL-SETUP; UE FRDGBuilder::AddSetupTask over
    // UE::Tasks, FParallelMeshDrawCommandPass). The render thread launches pure setup work - a plan computed by
    // index into a slot of its own - joins, and then applies the plans serially in a fixed order, so the frame
    // does not depend on which thread ran what. A task touches no shared member state and never the render graph
    // (RDG::Builder's entry points check InTask() and say so with a "[RenderTask]" line).
    //
    // Launch() submits a High job (JobPriority) per task. Join() CLAIMS AND RUNS every task no worker has started
    // yet on the calling thread, and only then waits - and only for tasks a worker is already running - so the
    // join never waits on a queued job (the ParallelRanges argument: no deadlock when every worker is busy, and a
    // task may itself run a TaskGroup or a ParallelFor). A task body is noexcept by contract (UE::Tasks and
    // AddSetupTask carry no exceptions): a failure is a value in the task's own plan slot, and a throw ends the
    // process at the throw site, crash dump there. Parallel off (RenderPipeline.ParallelSetup 0): Launch runs
    // the task at once on the calling thread, in launch order - the same plans, the A/B and the rollback.
    class TaskGroup
    {
    public:
        explicit TaskGroup( bool parallel );
        ~TaskGroup(); // joins what was not joined (RAII, as std::jthread): the tasks write the launcher's frames
        TaskGroup( const TaskGroup& )            = delete;
        TaskGroup& operator=( const TaskGroup& ) = delete;

        /// @p name a string with static storage (the profiler's row for the task). @p task is noexcept by type
        /// (InlineJob): a capture up to InlineJob::kInlineSize bytes costs no heap.
        void Launch( const char* name, InlineJob task );

        /// THE MIN-BATCH LAUNCH (UE ParallelFor MinBatchSize, FParallelMeshDrawCommandPass): [0, @p count) in
        /// contiguous slices, one task each - as many as there are participants (WorkerCount() + 1, the joining
        /// thread runs one too), but never a slice under @p minBatch items, so a short list is not cut into tasks
        /// that cost more to hand out than to run. body(first, last) owns [first, last); slice s is
        /// [count*s/n, count*(s+1)/n), so each item is in exactly one slice and in order within it. Off
        /// (Parallel() false) or a single slice: body(0, count) runs here, at once. The tasks go to the pool in
        /// one SubmitBatch. @p minBatch 0 is read as 1. @p body is copied once into the group's shared state.
        template <typename F>
        void LaunchSlices( const char* name, size_t count, size_t minBatch, F&& body );

        /// The slice count LaunchSlices uses for @p count items at @p minBatch per slice (at least one).
        [[nodiscard]] static size_t SliceCount( size_t count, size_t minBatch ) noexcept;

        /// Runs every unstarted task here, then waits for the running ones.
        void Join() noexcept;

        [[nodiscard]] bool Parallel() const noexcept
        {
            return m_Parallel;
        }

        /// True on a thread that is running a TaskGroup task (a worker, or the joining thread inside Join).
        [[nodiscard]] static bool InTask() noexcept;

    private:
        struct Shared;
        // Appends @p tasks (named @p name) to the group and submits their claim jobs in one SubmitBatch.
        void LaunchBatch( const char* name, std::span<InlineJob> tasks );
        // Runs @p task on the calling thread inside a task scope (InTask, the profiler row @p name).
        void RunHere( const char* name, InlineJob& task ) noexcept;

        std::shared_ptr<Shared> m_Shared;
        bool                    m_Parallel = false;
        bool                    m_Joined   = true;
    };

    template <typename F>
    void TaskGroup::LaunchSlices( const char* name, const size_t count, const size_t minBatch, F&& body )
    {
        using D = std::decay_t<F>;
        static_assert( std::is_nothrow_invocable_v<D&, size_t, size_t>,
                       "a task is noexcept: declare the lambda noexcept and return its failure as a value" );
        if ( count == 0 )
            return;
        const size_t slices = SliceCount( count, minBatch );
        if ( !m_Parallel || slices == 1 )
        {
            // Here and now, under InTask as a launched task (Launch with the switch off): nothing outlives it.
            m_Joined = false;
            D         local( std::forward<F>( body ) );
            InlineJob task( [&local, count]() noexcept { local( size_t{ 0 }, count ); } );
            RunHere( name, task );
            return;
        }
        // One copy of the body for every slice: the slices hold it by shared_ptr (16 bytes + two indices, inline
        // in InlineJob), and the last slice to finish frees it on the thread that ran it.
        auto             shared = std::make_shared<D>( std::forward<F>( body ) );
        constexpr size_t kBatch = 64;
        InlineJob        tasks[kBatch];
        size_t           filled = 0;
        for ( size_t s = 0; s < slices; ++s )
        {
            const size_t first = count * s / slices;
            const size_t last  = count * ( s + 1 ) / slices;
            tasks[filled++]    = InlineJob( [shared, first, last]() noexcept { ( *shared )( first, last ); } );
            if ( filled == kBatch )
            {
                LaunchBatch( name, std::span<InlineJob>( tasks, filled ) );
                filled = 0;
            }
        }
        if ( filled > 0 )
            LaunchBatch( name, std::span<InlineJob>( tasks, filled ) );
    }
} // namespace Common
