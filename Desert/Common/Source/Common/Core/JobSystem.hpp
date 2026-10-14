#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
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

        void operator()()
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
            void ( *Invoke )( void* );
            void ( *MoveTo )( void* src, void* dst ); // dst is uninitialized; src is destroyed
            void ( *Destroy )( void* );
        };

        // std::destroy_at rather than an explicit `->~D()`: D is a lambda closure type here, and MSVC
        // rejects the destructor-call syntax on a template parameter naming a closure ("class has no
        // destructor called '~D'"). destroy_at is equivalent, standard, and accepted by both compilers.
        template <typename D>
        static constexpr Ops s_InlineOps = {
            []( void* s ) { ( *static_cast<D*>( s ) )(); },
            []( void* src, void* dst )
            {
                new ( dst ) D( std::move( *static_cast<D*>( src ) ) );
                std::destroy_at( static_cast<D*>( src ) );
            },
            []( void* s ) { std::destroy_at( static_cast<D*>( s ) ); } };

        template <typename D>
        static constexpr Ops s_HeapOps = {
            []( void* s ) { ( **static_cast<D**>( s ) )(); },
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

    // Engine-wide worker-thread pool — THE place for CPU-parallel work (asset cooking, LUT generation,
    // background mesh cooks, future parallel ECS). Replaces the ad-hoc one-off std::thread/std::async
    // sprinkled around the codebase so thread count stays bounded (workers = cores - 1) and work is
    // observable in one system.
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

        ~JobSystem();
        JobSystem( const JobSystem& )            = delete;
        JobSystem& operator=( const JobSystem& ) = delete;

        // Fire-and-forget. Closures <= InlineJob::kInlineSize bytes enqueue with ZERO heap allocations.
        // False when the pool is shutting down and the job was dropped unrun: a caller that counts its jobs in
        // flight (and waits for them) must take this one back.
        bool Submit( InlineJob job );

        // Submit with a result: returns a std::future for the callable's return value.
        template <typename F>
        auto Async( F&& fn ) -> std::future<std::invoke_result_t<F>>
        {
            using R   = std::invoke_result_t<F>;
            auto task = std::make_shared<std::packaged_task<R()>>( std::forward<F>( fn ) );
            auto fut  = task->get_future();
            Submit( [task] { ( *task )(); } );
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
         */
        void ParallelRanges( size_t count, size_t grain,
                             const std::function<void( size_t begin, size_t end )>& body );

        // Blocking parallel-for over [0, count): body(index). ParallelRanges with the range walked for
        // you and a grain of about a quarter of a participant's share, so the loop stays balanced when
        // the indices cost different amounts (importing files, running ECS systems — both do).
        void ParallelFor( size_t count, const std::function<void( size_t )>& body );

        size_t WorkerCount() const
        {
            return m_Workers.size();
        }

        // Pending + currently-running job count (approximate; for tests/diagnostics).
        size_t PendingJobs() const;

        // Stops accepting jobs, drains the queue and joins the workers. Idempotent.
        void Shutdown();

    private:
        JobSystem();
        void WorkerLoop();

        mutable std::mutex      m_Mutex;
        std::condition_variable m_CV;
        std::deque<InlineJob>   m_Queue;
        std::vector<std::thread>          m_Workers;
        size_t                            m_Running = 0; // jobs currently executing
        bool                              m_Stop    = false;
    };
} // namespace Common
