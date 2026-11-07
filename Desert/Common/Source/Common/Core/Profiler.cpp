#include <Common/Core/Math/Rounding.hpp>

#include <cstdint>
#include "Profiler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

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
            double   GpuAsyncSelfMs = 0.0; // the async compute queue's part of GpuSelfMs
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
                GpuAsyncSelfMs += other.GpuAsyncSelfMs;
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

                [[nodiscard]] bool Reserved( const uint32_t id ) const noexcept
                {
                    const uint32_t page = id / kPageRows;
                    return page < Pages.size() && Pages[page] != nullptr;
                }

                // Makes row @p id exist and Touched hold one slot per row there is, so Row never allocates: an
                // id goes into Touched once per drain (every add counts a call), so it never outgrows that.
                void Reserve( const uint32_t id )
                {
                    const uint32_t page = id / kPageRows;
                    if ( page >= Pages.size() )
                        Pages.resize( page + 1 );
                    if ( !Pages[page] )
                        Pages[page] = std::make_unique<Page>();
                    Touched.reserve( Pages.size() * kPageRows );
                }

                // Row @p id of a reserved page (Reserve), null for one never reserved. Never allocates.
                ScopeAccum* Row( const uint32_t id ) noexcept
                {
                    if ( !Reserved( id ) )
                        return nullptr;
                    ScopeAccum& row = ( *Pages[id / kPageRows] )[id % kPageRows];
                    if ( row.Empty() )
                        Touched.push_back( id ); // within the capacity Reserve made: no reallocation
                    return &row;
                }

                // Adds every touched row into @p merged (by id) and leaves the half empty.
                // @p touched gains each id that was empty in @p merged before this.
                void DrainInto( std::vector<ScopeAccum>& merged, std::vector<uint32_t>& touched )
                {
                    for ( const uint32_t id : Touched )
                    {
                        ScopeAccum& row = ( *Pages[id / kPageRows] )[id % kPageRows];
                        if ( id >= merged.size() )
                            merged.resize( id + 1 );
                        if ( merged[id].Empty() )
                            touched.push_back( id );
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

            // Row @p id in both halves (Half::Reserve). The thread's own call, under the publisher's lock: the
            // publisher drains a half under it, the only time anyone but this thread reads a half.
            [[nodiscard]] bool Reserved( const uint32_t id ) const noexcept
            {
                return Halves[0].Reserved( id ); // Reserve grows both halves together
            }
            void Reserve( const uint32_t id )
            {
                for ( Half& half : Halves )
                    half.Reserve( id );
            }

            // Adds into row @p id of the active half; false (nothing added) for a row never reserved. @p add
            // must not throw: this runs in ScopedTimer's destructor.
            template <class Add>
            bool Write( const uint32_t id, Add&& add ) noexcept
            {
                Writing.store( true, std::memory_order_seq_cst );
                Half&       half = Halves[Active.load( std::memory_order_seq_cst )];
                ScopeAccum* row  = half.Row( id );
                if ( row != nullptr )
                    add( half, *row );
                Writing.store( false, std::memory_order_release );
                return row != nullptr;
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

        // THE WINDOW BEING ACCUMULATED (Profiler::BeginFrame): every thread's samples drained into it once a
        // frame, so a row knows each frame's cost (ScopeResult::MaxMs / P99Ms) and a frame its own largest
        // self rows (HitchFrame). Written by the frame thread alone.
        struct WindowState
        {
            struct FrameRecord
            {
                double                                 FrameMs = 0.0; // wall, BeginFrame to BeginFrame
                std::array<uint32_t, kHitchTopScopes> Ids{};
                std::array<double, kHitchTopScopes>   SelfMs{};
                uint32_t                               Count = 0;

                // Keeps the kHitchTopScopes largest self times, largest first.
                void Offer( const uint32_t id, const double selfMs ) noexcept
                {
                    if ( Count == kHitchTopScopes && selfMs <= SelfMs[Count - 1] )
                        return;
                    uint32_t at = Count < kHitchTopScopes ? Count++ : Count - 1;
                    for ( ; at > 0 && SelfMs[at - 1] < selfMs; --at )
                    {
                        Ids[at]    = Ids[at - 1];
                        SelfMs[at] = SelfMs[at - 1];
                    }
                    Ids[at]    = id;
                    SelfMs[at] = selfMs;
                }
            };

            std::vector<ScopeAccum>                        Frame; // the frame being drained, by id
            std::vector<uint32_t>                          FrameTouched;
            std::vector<ScopeAccum>                        Accum; // the window, by id
            std::vector<uint32_t>                          Touched;
            std::vector<std::vector<float>>                RowFrames; // by id: cpu ms of each frame it ran in
            std::vector<FrameRecord>                       Frames;
            std::vector<double>                            FrameMsScratch;
            std::vector<uint32_t>                          HitchScratch;
            double                                         NamedMs = 0.0; // the frame thread's outermost ms
            std::chrono::high_resolution_clock::time_point LastFrameAt{};
            bool                                           HaveLastFrame = false;
        };
    } // namespace Detail

    namespace
    {
        // The @p q quantile (nearest rank) of a row's per-frame cost over @p frames frames: @p sorted holds the
        // frames it ran in, ascending; every other frame of the window cost it 0.
        double QuantileWithIdle( const std::vector<float>& sorted, const uint32_t frames, const double q )
        {
            const uint64_t n    = std::max<uint64_t>( frames, sorted.size() );
            const auto     rank = std::max<uint64_t>( 1, static_cast<uint64_t>( std::ceil( q * double( n ) ) ) );
            const uint64_t idle = n - sorted.size();
            if ( rank <= idle )
                return 0.0;
            return static_cast<double>( sorted[static_cast<size_t>( rank - idle - 1 )] );
        }

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
        if ( !m_Window )
            m_Window = std::make_unique<Detail::WindowState>();
        Detail::WindowState& window = *m_Window;

        const auto now = Clock::now();
        if ( !m_WindowStarted )
        {
            m_WindowStart   = now;
            m_WindowStarted = true;
        }
        ++m_WindowFrames;

        // THE FRAME DRAIN (UE FThreadStats: per-thread packets merged at the frame's end): every thread's half
        // of the frame that just ended, by name id - once a frame, not once a window, so a row knows each frame's
        // cost and a frame its own largest rows. A thread that exited since the last drain left both halves
        // (nobody flips a retired buffer's halves again) and is dropped here.
        {
            std::lock_guard<std::mutex> lock( m_ThreadsMutex );
            for ( auto it = m_Threads.begin(); it != m_Threads.end(); )
            {
                Detail::ThreadStats& stats   = **it;
                const bool           retired = stats.Retired.load( std::memory_order_acquire );
                if ( retired )
                {
                    for ( Detail::ThreadStats::Half& half : stats.Halves )
                        half.DrainInto( window.Frame, window.FrameTouched );
                    it = m_Threads.erase( it );
                    continue;
                }
                Detail::ThreadStats::Half& half = stats.Flip();
                half.DrainInto( window.Frame, window.FrameTouched );
                if ( &stats == frameStats )
                    window.NamedMs += half.OutermostMs;
                half.OutermostMs = 0.0;
                ++it;
            }
        }

        // The drained frame into the window: its rows' sums, each row's cost this frame, its top self rows.
        {
            Detail::WindowState::FrameRecord record;
            if ( window.HaveLastFrame )
                record.FrameMs = std::chrono::duration<double, std::milli>( now - window.LastFrameAt ).count();
            if ( window.Accum.size() < window.Frame.size() )
            {
                window.Accum.resize( window.Frame.size() );
                window.RowFrames.resize( window.Frame.size() );
            }
            for ( const uint32_t id : window.FrameTouched )
            {
                Detail::ScopeAccum& row = window.Frame[id];
                if ( window.Accum[id].Empty() )
                    window.Touched.push_back( id );
                window.Accum[id] += row;
                if ( row.Calls > 0 )
                {
                    window.RowFrames[id].push_back( static_cast<float>( row.TotalMs ) );
                    record.Offer( id, row.SelfMs );
                }
                row = Detail::ScopeAccum{};
            }
            window.FrameTouched.clear();
            if ( window.HaveLastFrame )
                window.Frames.push_back( record );
            window.LastFrameAt   = now;
            window.HaveLastFrame = true;
        }

        const double windowMs    = std::chrono::duration<double, std::milli>( now - m_WindowStart ).count();
        const double windowLimit = static_cast<double>( m_AvgWindowSec ) * 1000.0;
        // A capture (BeginCapture / EndCapture) closes the window by its frame span, never by time.
        const CaptureState capture    = m_Capture.load( std::memory_order_acquire );
        const bool         forceClose = capture == CaptureState::Arming || capture == CaptureState::Ending;
        const bool         holdOpen   = capture == CaptureState::Capturing || capture == CaptureState::Holding;
        if ( !forceClose && ( holdOpen || windowMs < windowLimit || m_WindowFrames == 0 ) )
            return; // still accumulating this window

        // Window elapsed: publish the AVERAGE per-frame cost of every scope over the window.
        const double   inv    = 1.0 / static_cast<double>( m_WindowFrames );
        const uint32_t frames = m_WindowFrames;
        m_Display.clear();
        {
            std::lock_guard<std::mutex> lock( m_NameMutex );
            for ( const uint32_t id : window.Touched )
            {
                Detail::ScopeAccum& result = window.Accum[id];
                ScopeResult         avg;
                avg.Name    = m_NameText[id];
                avg.TotalMs = result.TotalMs * inv;                                              // avg ms / frame
                avg.Calls = static_cast<uint32_t>( Math::RoundToNearest( result.Calls * inv ) ); // avg calls/frame
                avg.SelfMs    = result.SelfMs * inv;
                avg.WaitMs    = result.WaitMs * inv;
                avg.GpuMs     = result.GpuMs * inv; // avg GPU ms / frame
                avg.GpuSelfMs = result.GpuSelfMs * inv;
                avg.GpuAsyncSelfMs = result.GpuAsyncSelfMs * inv;
                avg.GpuCalls  = static_cast<uint32_t>( Math::RoundToNearest( result.GpuCalls * inv ) );
                std::vector<float>& perFrame = window.RowFrames[id];
                if ( !perFrame.empty() )
                {
                    std::sort( perFrame.begin(), perFrame.end() );
                    avg.MaxMs = static_cast<double>( perFrame.back() );
                    avg.P99Ms = QuantileWithIdle( perFrame, frames, 0.99 );
                    perFrame.clear();
                }
                result = Detail::ScopeAccum{};
                m_Display.push_back( std::move( avg ) );
            }
            window.Touched.clear();

            // THE HITCHES: the window's frames over kHitchFactor x its median wall time, slowest first.
            m_DisplayHitches.clear();
            m_DisplayHitchCount = 0;
            m_DisplayMedianMs   = 0.0;
            if ( !window.Frames.empty() )
            {
                window.FrameMsScratch.clear();
                for ( const Detail::WindowState::FrameRecord& record : window.Frames )
                    window.FrameMsScratch.push_back( record.FrameMs );
                const auto middle = window.FrameMsScratch.begin() +
                                    static_cast<std::ptrdiff_t>( window.FrameMsScratch.size() / 2 );
                std::nth_element( window.FrameMsScratch.begin(), middle, window.FrameMsScratch.end() );
                m_DisplayMedianMs = *middle;
                window.HitchScratch.clear();
                for ( uint32_t f = 0; f < window.Frames.size(); ++f )
                    if ( m_DisplayMedianMs > 0.0 && window.Frames[f].FrameMs > kHitchFactor * m_DisplayMedianMs )
                        window.HitchScratch.push_back( f );
                m_DisplayHitchCount = static_cast<uint32_t>( window.HitchScratch.size() );
                std::sort( window.HitchScratch.begin(), window.HitchScratch.end(),
                           [&window]( uint32_t a, uint32_t b )
                           { return window.Frames[a].FrameMs > window.Frames[b].FrameMs; } );
                if ( window.HitchScratch.size() > kMaxPublishedHitches )
                    window.HitchScratch.resize( kMaxPublishedHitches );
                for ( const uint32_t f : window.HitchScratch )
                {
                    const Detail::WindowState::FrameRecord& record = window.Frames[f];
                    HitchFrame                              hitch;
                    hitch.FrameMs  = record.FrameMs;
                    hitch.MedianMs = m_DisplayMedianMs;
                    hitch.Frame    = f;
                    for ( uint32_t k = 0; k < record.Count; ++k )
                        hitch.Top.emplace_back( m_NameText[record.Ids[k]], record.SelfMs[k] );
                    m_DisplayHitches.push_back( std::move( hitch ) );
                }
                window.Frames.clear();
            }
        }
        if ( m_SortByTime )
            std::sort( m_Display.begin(), m_Display.end(),
                       []( const ScopeResult& a, const ScopeResult& b ) { return a.TotalMs > b.TotalMs; } );
        else
            std::sort( m_Display.begin(), m_Display.end(),
                       []( const ScopeResult& a, const ScopeResult& b ) { return a.Name < b.Name; } );

        m_DisplayFrameMs      = windowMs * inv; // avg frame time over the window
        m_DisplayFrameNamedMs = window.NamedMs * inv;
        window.NamedMs        = 0.0;

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

    bool Profiler::ReserveSample( const uint32_t nameId )
    {
        if ( !m_Enabled || nameId == kNoScopeName )
            return false;
        Detail::ThreadStats* const stats = LocalStats();
        if ( stats == nullptr )
            return false;
        // Only this thread grows its halves, so it reads them without a lock; it grows them under the publisher's
        // lock, the only other reader (BeginFrame drains a half under it). Once per page of rows per thread.
        if ( !stats->Reserved( nameId ) )
        {
            const std::lock_guard<std::mutex> lock( m_ThreadsMutex );
            stats->Reserve( nameId );
        }
        return true;
    }

    void Profiler::AddSample( const uint32_t nameId, const double totalMs, const double selfMs,
                              const double waitMs, const bool outermost ) noexcept
    {
        if ( !m_Enabled || nameId == kNoScopeName )
            return;
        // t_Stats, not LocalStats: the buffer exists since ReserveSample, and making one would allocate.
        Detail::ThreadStats* const stats = t_Stats;
        if ( stats == nullptr )
            return;

        // Summed across the whole window in this thread's half, merged and averaged on publish. A row not
        // reserved (the profiler was off when the scope opened) is not counted: the scope began unmeasured.
        (void)stats->Write( nameId,
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

    // EVERYTHING THAT CAN ALLOCATE HAPPENS WHEN THE SCOPE OPENS: the runtime name's id (this thread's name
    // cache), the thread's buffer and the row in both its halves (ReserveSample). The destructor then only does
    // arithmetic and AddSample (noexcept). A scope opened while the profiler is off keeps kNoScopeName and is
    // not measured; m_Start is taken last, so the lookup is not this scope's own time.
    ScopedTimer::ScopedTimer( const char* name, ScopeKind kind )
         : m_Enclosing( t_Innermost ), m_Kind( kind )
    {
        Profiler& profiler = Profiler::Get();
        if ( name != nullptr && profiler.Enabled() )
        {
            const uint32_t id = profiler.LocalScopeName( {}, name ).Id;
            if ( profiler.ReserveSample( id ) )
                m_NameId = id;
        }
        t_Innermost = this;
        m_Start     = std::chrono::high_resolution_clock::now();
    }

    ScopedTimer::ScopedTimer( const InternedScopeName& name, ScopeKind kind )
         : m_Enclosing( t_Innermost ), m_Kind( kind )
    {
        if ( Profiler::Get().ReserveSample( name.Id ) )
            m_NameId = name.Id;
        t_Innermost = this;
        m_Start     = std::chrono::high_resolution_clock::now();
    }

    ScopedTimer::~ScopedTimer() noexcept
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

        if ( m_NameId != kNoScopeName )
            Profiler::Get().AddSample( m_NameId, totalMs, std::max( 0.0, totalMs - m_NestedMs ), waitMs,
                                       m_Enclosing == nullptr );
    }

    void Profiler::AddGpuSample( const char* name, double inclusiveMs, double selfMs )
    {
        if ( !m_Enabled || name == nullptr )
            return;

        // The backend resolves a frame's queries MaxFramesInFlight frames after they were written, so this
        // lands in a LATER window than the CPU sample of the same scope. Over a window of hundreds of
        // frames that shifts the average by the lag divided by the window — well under a per cent — and it
        // keeps the read off the GPU's critical path, which is the whole point.
        AddGpuSample( LocalScopeName( {}, name ).Id, inclusiveMs, selfMs, GpuQueue::Graphics );
    }

    void Profiler::AddGpuSample( const uint32_t nameId, const double inclusiveMs, const double selfMs,
                                 const GpuQueue queue )
    {
        if ( !ReserveSample( nameId ) )
            return;
        const double asyncSelfMs = queue == GpuQueue::AsyncCompute ? selfMs : 0.0;
        (void)t_Stats->Write( nameId,
                              [&]( Detail::ThreadStats::Half&, Detail::ScopeAccum& row )
                              {
                                  row.GpuMs += inclusiveMs;
                                  row.GpuSelfMs += selfMs;
                                  row.GpuAsyncSelfMs += asyncSelfMs;
                                  row.GpuCalls += 1;
                              } );
    }

    const char* Profiler::ScopeNameText( const uint32_t id )
    {
        std::lock_guard<std::mutex> lock( m_NameMutex );
        return id < m_NameText.size() ? m_NameText[id].c_str() : "";
    }

#if USE_OPTICK
    ::Optick::EventDescription* Profiler::OptickEventOf( const InternedScopeName& name )
    {
        thread_local std::vector<::Optick::EventDescription*> t_Events;
        if ( name.Id == kNoScopeName || name.Text == nullptr )
        {
            static ::Optick::EventDescription* const s_Unnamed =
                 ::Optick::EventDescription::Create( "(unnamed scope)", __FILE__, __LINE__ );
            return s_Unnamed;
        }
        if ( name.Id < t_Events.size() && t_Events[name.Id] != nullptr )
            return t_Events[name.Id];
        ::Optick::EventDescription* made = nullptr;
        {
            std::lock_guard<std::mutex> lock( m_NameMutex );
            if ( name.Id >= m_OptickEvents.size() )
                m_OptickEvents.resize( name.Id + 1, nullptr );
            if ( m_OptickEvents[name.Id] == nullptr ) // the table's text lives as long as the process
                m_OptickEvents[name.Id] = ::Optick::EventDescription::Create( name.Text, __FILE__, __LINE__ );
            made = m_OptickEvents[name.Id];
        }
        if ( name.Id >= t_Events.size() )
            t_Events.resize( name.Id + 1, nullptr );
        t_Events[name.Id] = made;
        return made;
    }
#endif

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
