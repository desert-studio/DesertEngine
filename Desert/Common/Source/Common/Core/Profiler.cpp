#include <Common/Core/Math/Rounding.hpp>

#include <cstdint>
#include "Profiler.hpp"

#include <algorithm>
#include <array>
#include <atomic>

namespace Common::Profiling
{
    namespace Detail
    {
        // One row of one thread between two publications: the sums ScopeResult averages.
        struct ScopeAccum
        {
            double   TotalMs   = 0.0;
            double   SelfMs    = 0.0;
            double   WaitMs    = 0.0;
            double   GpuMs     = 0.0;
            double   GpuSelfMs = 0.0;
            uint32_t Calls     = 0;
            uint32_t GpuCalls  = 0;

            // A row is part of a window exactly when something was added to it: every add counts a call.
            [[nodiscard]] bool Empty() const noexcept
            {
                return Calls == 0 && GpuCalls == 0;
            }
            ScopeAccum& operator+=( const ScopeAccum& other ) noexcept
            {
                TotalMs += other.TotalMs;
                SelfMs += other.SelfMs;
                WaitMs += other.WaitMs;
                GpuMs += other.GpuMs;
                GpuSelfMs += other.GpuSelfMs;
                Calls += other.Calls;
                GpuCalls += other.GpuCalls;
                return *this;
            }
        };

        // THE THREAD-NAME CACHE'S KEY: a name by its two parts (LocalScopeName), hashed and compared as the
        // concatenation it stands for, so a lookup builds no string. FNV-1a, because it streams: hashing the
        // prefix and then the name gives exactly the hash of the stored concatenation.
        struct ScopeNameKey
        {
            std::string_view Prefix;
            std::string_view Name;
        };

        struct ScopeNameKeyHash
        {
            using is_transparent = void;

            static constexpr uint64_t kOffset = 14695981039346656037ull;
            static constexpr uint64_t kPrime  = 1099511628211ull;

            [[nodiscard]] static uint64_t Mix( uint64_t hash, const std::string_view text ) noexcept
            {
                for ( const char c : text )
                {
                    hash ^= static_cast<unsigned char>( c );
                    hash *= kPrime;
                }
                return hash;
            }
            size_t operator()( const ScopeNameKey& key ) const noexcept
            {
                return static_cast<size_t>( Mix( Mix( kOffset, key.Prefix ), key.Name ) );
            }
            size_t operator()( const std::string& text ) const noexcept
            {
                return static_cast<size_t>( Mix( kOffset, text ) );
            }
        };

        struct ScopeNameKeyEqual
        {
            using is_transparent = void;

            [[nodiscard]] static bool Matches( const std::string_view text, const ScopeNameKey& key ) noexcept
            {
                return text.size() == key.Prefix.size() + key.Name.size() && text.starts_with( key.Prefix ) &&
                       text.substr( key.Prefix.size() ) == key.Name;
            }
            bool operator()( const std::string& a, const std::string& b ) const noexcept
            {
                return a == b;
            }
            bool operator()( const ScopeNameKey& key, const std::string& text ) const noexcept
            {
                return Matches( text, key );
            }
            bool operator()( const std::string& text, const ScopeNameKey& key ) const noexcept
            {
                return Matches( text, key );
            }
        };

        // ONE THREAD'S SAMPLES (UE FThreadStats), in two halves. The thread adds into the ACTIVE half and never
        // takes a lock; the publisher (BeginFrame) makes the other half active and then reads and clears the one
        // the thread has stopped writing. The handover is a Dekker pair on two atomics: the thread raises
        // Writing BEFORE it reads Active, the publisher stores Active BEFORE it reads Writing, all four
        // sequentially consistent - so either the thread already sees the new half, or the publisher sees the
        // thread inside an add on the old one and waits the few nanoseconds that add takes.
        struct ThreadStats
        {
            struct Half
            {
                // Rows by name id, in pages, so a thread that only ever touched a few late-interned names does
                // not hold a row for every name before them.
                static constexpr uint32_t kPageRows = 64;
                using Page                          = std::array<ScopeAccum, kPageRows>;

                std::vector<std::unique_ptr<Page>> Pages;
                std::vector<uint32_t>              Touched;           // ids added to in this half, for the merge
                double                             OutermostMs = 0.0; // see Profiler::LastFrameNamedMs

                ScopeAccum& Row( const uint32_t id )
                {
                    const uint32_t page = id / kPageRows;
                    if ( page >= Pages.size() )
                        Pages.resize( page + 1 );
                    if ( !Pages[page] )
                        Pages[page] = std::make_unique<Page>();
                    ScopeAccum& row = ( *Pages[page] )[id % kPageRows];
                    if ( row.Empty() )
                        Touched.push_back( id );
                    return row;
                }

                // Adds every touched row into @p merged (by id) and leaves the half empty.
                void DrainInto( std::vector<ScopeAccum>& merged )
                {
                    for ( const uint32_t id : Touched )
                    {
                        ScopeAccum& row = ( *Pages[id / kPageRows] )[id % kPageRows];
                        if ( id >= merged.size() )
                            merged.resize( id + 1 );
                        merged[id] += row;
                        row = ScopeAccum{};
                    }
                    Touched.clear();
                }
            };

            std::array<Half, 2>   Halves;
            std::atomic<uint32_t> Active{ 0 };
            std::atomic<bool>     Writing{ false };
            // Set by the thread's exit (ThreadStatsRetirer) after its last add: from then on nobody writes either
            // half, and the next publication drains both and drops the buffer.
            std::atomic<bool> Retired{ false };

            // This thread's runtime names (LocalScopeName). Read and written by this thread alone.
            std::unordered_map<std::string, InternedScopeName, ScopeNameKeyHash, ScopeNameKeyEqual> Names;

            template <class Add>
            void Write( const uint32_t id, Add&& add )
            {
                Writing.store( true, std::memory_order_seq_cst );
                Half& half = Halves[Active.load( std::memory_order_seq_cst )];
                add( half, half.Row( id ) );
                Writing.store( false, std::memory_order_release );
            }

            // The publisher's side of the handover: the half this thread wrote until now, no longer written.
            Half& Flip()
            {
                const uint32_t old = Active.load( std::memory_order_relaxed ); // only the publisher stores it
                Active.store( old ^ 1u, std::memory_order_seq_cst );
                while ( Writing.load( std::memory_order_seq_cst ) )
                    std::this_thread::yield();
                return Halves[old];
            }
        };
    } // namespace Detail

    namespace
    {
        // The calling thread's buffer (Profiler::LocalStats). Trivially destructible, so it can be read at any
        // point of the thread's exit, including after the retirer below has run.
        thread_local Detail::ThreadStats* t_Stats        = nullptr;
        thread_local bool                 t_StatsRetired = false;

        // Retires the thread's buffer when the thread exits: the last thing the thread does to it. A sample
        // closed after that (a thread_local destroyed later that times its teardown) is dropped, never written
        // into a buffer the publisher may already have freed.
        struct ThreadStatsRetirer
        {
            ThreadStatsRetirer()                                       = default;
            ThreadStatsRetirer( const ThreadStatsRetirer& )            = delete;
            ThreadStatsRetirer& operator=( const ThreadStatsRetirer& ) = delete;
            ~ThreadStatsRetirer()
            {
                if ( t_Stats != nullptr )
                    t_Stats->Retired.store( true, std::memory_order_release );
                t_Stats        = nullptr;
                t_StatsRetired = true;
            }
        };
    } // namespace

    Profiler& Profiler::Get()
    {
        static Profiler s_Instance;
        return s_Instance;
    }

    Profiler::Profiler() = default;

    Profiler::~Profiler()
    {
        // The singleton dies in static destruction, on the thread that called exit(). Some runtimes destroy that
        // thread's thread_locals AFTER the statics, so its retirer would mark a buffer this destructor is about to
        // free: detach the thread from it first. Every other thread has been joined by now.
        t_Stats        = nullptr;
        t_StatsRetired = true;
    }

    Detail::ThreadStats* Profiler::LocalStats()
    {
        if ( t_Stats != nullptr )
            return t_Stats;
        if ( t_StatsRetired )
            return nullptr;

        auto                 owned = std::make_unique<Detail::ThreadStats>();
        Detail::ThreadStats* stats = owned.get();
        {
            std::lock_guard<std::mutex> lock( m_ThreadsMutex );
            m_Threads.push_back( std::move( owned ) );
        }
        // Function-local, so it is constructed - and its destructor registered for this thread's exit - on the
        // thread's first sample, and never for a thread that closes no scope.
        static thread_local ThreadStatsRetirer retirer;
        (void)retirer;
        t_Stats = stats;
        return stats;
    }

    InternedScopeName Profiler::InternScopeName( const std::string_view name )
    {
        std::lock_guard<std::mutex> lock( m_NameMutex );
        if ( const auto found = m_NameIds.find( name ); found != m_NameIds.end() )
            return { found->second, m_NameText[found->second].c_str() };
        const auto         id   = static_cast<uint32_t>( m_NameText.size() );
        const std::string& text = m_NameText.emplace_back( name );
        m_NameIds.emplace( std::string_view( text ), id );
        return { id, text.c_str() };
    }

    InternedScopeName Profiler::LocalScopeName( const std::string_view prefix, const std::string_view name )
    {
        Detail::ThreadStats* const stats = LocalStats();
        const Detail::ScopeNameKey key{ prefix, name };
        if ( stats != nullptr )
            if ( const auto found = stats->Names.find( key ); found != stats->Names.end() )
                return found->second;

        // First sight of this name on this thread: the one time it is concatenated.
        std::string text;
        text.reserve( prefix.size() + name.size() );
        text.append( prefix ).append( name );
        const InternedScopeName interned = InternScopeName( text );
        if ( stats != nullptr )
            stats->Names.emplace( std::move( text ), interned );
        return interned;
    }

    void Profiler::BeginFrame()
    {
        Detail::ThreadStats* const frameStats = LocalStats(); // the caller is the frame thread

        const auto now = Clock::now();
        if ( !m_WindowStarted )
        {
            m_WindowStart   = now;
            m_WindowStarted = true;
        }
        ++m_WindowFrames;

        const double windowMs    = std::chrono::duration<double, std::milli>( now - m_WindowStart ).count();
        const double windowLimit = static_cast<double>( m_AvgWindowSec ) * 1000.0;
        // A capture (BeginCapture / EndCapture) closes the window by its frame span, never by time.
        const CaptureState capture    = m_Capture.load( std::memory_order_acquire );
        const bool         forceClose = capture == CaptureState::Arming || capture == CaptureState::Ending;
        const bool         holdOpen   = capture == CaptureState::Capturing || capture == CaptureState::Holding;
        if ( !forceClose && ( holdOpen || windowMs < windowLimit || m_WindowFrames == 0 ) )
            return; // still accumulating this window

        // THE MERGE: every thread's half of the window, by name id. A thread that exited since the last
        // publication left both halves (nobody flips a retired buffer's halves again) and is dropped here.
        std::vector<Detail::ScopeAccum> merged;
        double                          frameNamedMs = 0.0;
        {
            std::lock_guard<std::mutex> lock( m_ThreadsMutex );
            for ( auto it = m_Threads.begin(); it != m_Threads.end(); )
            {
                Detail::ThreadStats& stats   = **it;
                const bool           retired = stats.Retired.load( std::memory_order_acquire );
                if ( retired )
                {
                    for ( Detail::ThreadStats::Half& half : stats.Halves )
                        half.DrainInto( merged );
                    it = m_Threads.erase( it );
                    continue;
                }
                Detail::ThreadStats::Half& half = stats.Flip();
                half.DrainInto( merged );
                if ( &stats == frameStats )
                    frameNamedMs = half.OutermostMs;
                half.OutermostMs = 0.0;
                ++it;
            }
        }

        // Window elapsed: publish the AVERAGE per-frame cost of every scope over the window.
        const double inv = 1.0 / static_cast<double>( m_WindowFrames );
        m_Display.clear();
        {
            std::lock_guard<std::mutex> lock( m_NameMutex );
            for ( uint32_t id = 0; id < merged.size(); ++id )
            {
                const Detail::ScopeAccum& result = merged[id];
                if ( result.Empty() )
                    continue;
                ScopeResult avg;
                avg.Name    = m_NameText[id];
                avg.TotalMs = result.TotalMs * inv;                                              // avg ms / frame
                avg.Calls = static_cast<uint32_t>( Math::RoundToNearest( result.Calls * inv ) ); // avg calls/frame
                avg.SelfMs    = result.SelfMs * inv;
                avg.WaitMs    = result.WaitMs * inv;
                avg.GpuMs     = result.GpuMs * inv; // avg GPU ms / frame
                avg.GpuSelfMs = result.GpuSelfMs * inv;
                avg.GpuCalls  = static_cast<uint32_t>( Math::RoundToNearest( result.GpuCalls * inv ) );
                m_Display.push_back( std::move( avg ) );
            }
        }
        if ( m_SortByTime )
            std::sort( m_Display.begin(), m_Display.end(),
                       []( const ScopeResult& a, const ScopeResult& b ) { return a.TotalMs > b.TotalMs; } );
        else
            std::sort( m_Display.begin(), m_Display.end(),
                       []( const ScopeResult& a, const ScopeResult& b ) { return a.Name < b.Name; } );

        m_DisplayFrameMs      = windowMs * inv; // avg frame time over the window
        m_DisplayFrameNamedMs = frameNamedMs * inv;

        // The mesh draw split's groups, averaged over the same frames (empty when the split was off all window).
        {
            std::lock_guard<std::mutex> lock( m_Mutex );
            m_DrawSplitDisplay.clear();
            m_DrawSplitDisplay.reserve( m_DrawSplitAccum.size() );
            for ( const auto& [key, group] : m_DrawSplitAccum )
            {
                DrawSplitResult avg = group;
                avg.GpuMs *= inv;
                avg.Draws *= inv;
                avg.Triangles *= inv;
                m_DrawSplitDisplay.push_back( std::move( avg ) );
            }
            m_DrawSplitAccum.clear();
        }
        std::sort( m_DrawSplitDisplay.begin(), m_DrawSplitDisplay.end(),
                   []( const DrawSplitResult& a, const DrawSplitResult& b )
                   { return a.Pass != b.Pass ? a.Pass < b.Pass : a.GpuMs > b.GpuMs; } );

        ++m_PublishCount;
        m_DisplayWindowFrames = m_WindowFrames;
        m_DisplayClosedAt     = Clock::now();
        m_DisplayCaptured     = capture == CaptureState::Ending;
        m_WindowFrames = 0;
        m_WindowStart  = now;
        // Advance the capture only from the state this publication acted on: a BeginCapture that arrived since
        // the load above re-arms, and the next BeginFrame honours it.
        CaptureState acted = capture;
        if ( capture == CaptureState::Arming )
            m_Capture.compare_exchange_strong( acted, CaptureState::Capturing, std::memory_order_acq_rel );
        else if ( capture == CaptureState::Ending )
            m_Capture.compare_exchange_strong( acted, CaptureState::Holding, std::memory_order_acq_rel );
    }

    void Profiler::BeginCapture()
    {
        m_Capture.store( CaptureState::Arming, std::memory_order_release );
    }

    void Profiler::EndCapture()
    {
        // A capture shorter than one frame (still Arming) ends as well: its window is then the one frame.
        const CaptureState state = m_Capture.load( std::memory_order_acquire );
        if ( state == CaptureState::Capturing || state == CaptureState::Arming )
            m_Capture.store( CaptureState::Ending, std::memory_order_release );
    }

    void Profiler::ReleaseCapture()
    {
        CaptureState expected = CaptureState::Holding;
        m_Capture.compare_exchange_strong( expected, CaptureState::None, std::memory_order_acq_rel );
    }

    void Profiler::AddSample( const uint32_t nameId, const double totalMs, const double selfMs,
                              const double waitMs, const bool outermost )
    {
        if ( !m_Enabled || nameId == kNoScopeName )
            return;
        Detail::ThreadStats* const stats = LocalStats();
        if ( stats == nullptr )
            return;

        // Summed across the whole window in this thread's half, merged and averaged on publish.
        stats->Write( nameId,
                      [&]( Detail::ThreadStats::Half& half, Detail::ScopeAccum& row )
                      {
                          if ( outermost )
                              half.OutermostMs += totalMs;
                          row.TotalMs += totalMs;
                          row.SelfMs += selfMs;
                          row.WaitMs += waitMs;
                          row.Calls += 1;
                      } );
    }

    namespace
    {
        // The innermost open ScopedTimer of this thread: the top of the thread's scope stack, linked through
        // ScopedTimer::m_Enclosing. Per thread, so a job's scopes on a worker never become children of
        // whatever the main thread has open.
        thread_local ScopedTimer* t_Innermost = nullptr;
    } // namespace

    ScopedTimer::ScopedTimer( const char* name, ScopeKind kind )
         : m_Name( name ), m_Enclosing( t_Innermost ), m_Start( std::chrono::high_resolution_clock::now() ),
           m_Kind( kind )
    {
        t_Innermost = this;
    }

    ScopedTimer::ScopedTimer( const InternedScopeName& name, ScopeKind kind )
         : m_Name( name.Text ), m_NameId( name.Id ), m_Enclosing( t_Innermost ),
           m_Start( std::chrono::high_resolution_clock::now() ), m_Kind( kind )
    {
        t_Innermost = this;
    }

    ScopedTimer::~ScopedTimer()
    {
        // A BODY-LEVEL try, not a function-try-block, and the difference is not style. A function-try-block
        // on a destructor also covers the destruction of bases and members, none of which can throw here;
        // what it DOES do is put a `try` between the class body and the brace, and the pointer-ownership
        // census tracks members by brace depth (measured: it stopped seeing the members entirely).
        try
        {
            const double totalMs =
                 std::chrono::duration<double, std::milli>( std::chrono::high_resolution_clock::now() - m_Start )
                      .count();
            // A Wait scope is wait from end to end; a Work scope reports the waits nested in it.
            const double waitMs = m_Kind == ScopeKind::Wait ? totalMs : m_WaitMs;

            // Off the stack. The innermost timer in the ordinary case; otherwise a timer opened after this one
            // is still open (it outlives its parent) and takes this one's enclosing timer as its own.
            if ( t_Innermost == this )
                t_Innermost = m_Enclosing;
            else
                for ( ScopedTimer* inner = t_Innermost; inner != nullptr; inner = inner->m_Enclosing )
                    if ( inner->m_Enclosing == this )
                    {
                        inner->m_Enclosing = m_Enclosing;
                        break;
                    }
            if ( m_Enclosing != nullptr )
            {
                m_Enclosing->m_NestedMs += totalMs;
                m_Enclosing->m_WaitMs += waitMs;
            }

            Profiler& profiler = Profiler::Get();
            if ( profiler.Enabled() && m_Name != nullptr )
            {
                // An interned name is its id already; a runtime one is found in this thread's name cache.
                const uint32_t nameId =
                     m_NameId != kNoScopeName ? m_NameId : profiler.LocalScopeName( {}, m_Name ).Id;
                profiler.AddSample( nameId, totalMs, std::max( 0.0, totalMs - m_NestedMs ), waitMs,
                                    m_Enclosing == nullptr );
            }
        }
        DESERT_DESTRUCTOR_GUARD( "~ScopedTimer" )
    }

    void Profiler::AddGpuSample( const char* name, double inclusiveMs, double selfMs )
    {
        if ( !m_Enabled || name == nullptr )
            return;

        // The backend resolves a frame's queries MaxFramesInFlight frames after they were written, so this
        // lands in a LATER window than the CPU sample of the same scope. Over a window of hundreds of
        // frames that shifts the average by the lag divided by the window — well under a per cent — and it
        // keeps the read off the GPU's critical path, which is the whole point.
        const uint32_t             nameId = LocalScopeName( {}, name ).Id;
        Detail::ThreadStats* const stats  = LocalStats();
        if ( stats == nullptr )
            return;
        stats->Write( nameId,
                      [&]( Detail::ThreadStats::Half&, Detail::ScopeAccum& row )
                      {
                          row.GpuMs += inclusiveMs;
                          row.GpuSelfMs += selfMs;
                          row.GpuCalls += 1;
                      } );
    }

    void Profiler::AddDrawSplitSample( std::string_view pass, std::string_view mesh, std::string_view material,
                                       double gpuMs, const DrawSplitCounts& counts )
    {
        if ( !m_Enabled )
            return;

        std::string key;
        key.reserve( pass.size() + mesh.size() + material.size() + 2 );
        key.append( pass ).append( 1, '\x1f' ).append( mesh ).append( 1, '\x1f' ).append( material );

        std::lock_guard<std::mutex> lock( m_Mutex );
        DrawSplitResult&            group = m_DrawSplitAccum[key];
        if ( group.Pass.empty() )
        {
            group.Pass     = std::string( pass );
            group.Mesh     = std::string( mesh );
            group.Material = std::string( material );
        }
        group.GpuMs += gpuMs;
        group.Draws += static_cast<double>( counts.Draws );
        group.Triangles += static_cast<double>( counts.Triangles );
        group.TrianglesPerInstance = group.TrianglesPerInstance || counts.TrianglesPerInstance;
    }
} // namespace Common::Profiling
