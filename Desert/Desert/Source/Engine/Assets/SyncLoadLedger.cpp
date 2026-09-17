#include "SyncLoadLedger.hpp"

#include <Common/Core/Logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace Desert::Assets
{
    namespace
    {
        std::atomic<bool>     g_BootFinished{ false };
        std::atomic<uint64_t> g_Loads{ 0 };
        std::atomic<uint64_t> g_InFrameLoads{ 0 };

        // THE DEPTH IS THREAD-LOCAL, the totals are not, and the split is load-bearing. The preloader
        // runs staged work and hot reload runs off the watcher, so two threads can be inside a load at
        // once; a shared depth counter would let thread B's nested load be attributed as thread A's
        // outermost one, and the "outermost only" rule that keeps TotalMs under wall clock would break in
        // a way that shows up as a boot spending 250 % of its own duration.
        int& Depth()
        {
            static thread_local int depth = 0;
            return depth;
        }

        /// The innermost open scope on THIS thread, so a closing scope can hand its duration to its
        /// parent. Thread-local for the same reason the depth is: the preloader runs staged work and hot
        /// reload runs off the watcher, and a shared pointer here would let one thread's load be
        /// subtracted from another thread's.
        LoadTimingScope*& OpenScope()
        {
            static thread_local LoadTimingScope* open = nullptr;
            return open;
        }

        std::mutex& TotalsLock()
        {
            static std::mutex lock;
            return lock;
        }

        double      g_TotalMs   = 0.0;
        double      g_InFrameMs = 0.0;
        double      g_SlowestMs = 0.0;
        std::string g_SlowestPath;

        /// How many in-frame loads may log themselves per frame before the rest are counted silently.
        ///
        /// A CAP AND NOT A SWITCH. Fifty thousand entities touching their meshes in one frame would
        /// produce fifty thousand WARN lines, and a log that cannot be read is the same as no log — but
        /// so is a log that is off by default. Sixteen is enough to name the offenders (they repeat) and
        /// few enough to read. The suppressed count still goes out, because "16 loads" and "16 logged of
        /// 4000" are different facts.
        constexpr uint64_t    kInFrameLogCap = 16;
        std::atomic<uint64_t> g_InFrameLogged{ 0 };

        double NowMs( const int64_t ns )
        {
            return static_cast<double>( ns ) / 1'000'000.0;
        }

        int64_t NowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch() )
                 .count();
        }

        std::string Ms( const double ms )
        {
            char text[32];
            std::snprintf( text, sizeof( text ), "%.2f ms", ms );
            return text;
        }
    } // namespace

    void SyncLoadLedger::NoteBootFinished()
    {
        g_BootFinished.store( true, std::memory_order_relaxed );
    }

    LoadPhase SyncLoadLedger::Phase()
    {
        return g_BootFinished.load( std::memory_order_relaxed ) ? LoadPhase::Frame : LoadPhase::Boot;
    }

    uint64_t SyncLoadLedger::Loads()
    {
        return g_Loads.load( std::memory_order_relaxed );
    }

    uint64_t SyncLoadLedger::InFrameLoads()
    {
        return g_InFrameLoads.load( std::memory_order_relaxed );
    }

    double SyncLoadLedger::TotalMs()
    {
        std::lock_guard<std::mutex> guard( TotalsLock() );
        return g_TotalMs;
    }

    double SyncLoadLedger::InFrameMs()
    {
        std::lock_guard<std::mutex> guard( TotalsLock() );
        return g_InFrameMs;
    }

    double SyncLoadLedger::SlowestMs()
    {
        std::lock_guard<std::mutex> guard( TotalsLock() );
        return g_SlowestMs;
    }

    std::string SyncLoadLedger::SlowestPath()
    {
        std::lock_guard<std::mutex> guard( TotalsLock() );
        return g_SlowestPath;
    }

    void SyncLoadLedger::Record( const std::string& path, const double totalMs, const double selfMs,
                                 const bool outermost )
    {
        g_Loads.fetch_add( 1, std::memory_order_relaxed );

        const bool inFrame = Phase() == LoadPhase::Frame;
        if ( inFrame )
        {
            g_InFrameLoads.fetch_add( 1, std::memory_order_relaxed );
        }

        {
            std::lock_guard<std::mutex> guard( TotalsLock() );
            if ( outermost )
            {
                g_TotalMs += totalMs;
                if ( inFrame )
                    g_InFrameMs += totalMs;
            }
            // RANKED BY SELF TIME, AT EVERY DEPTH. Wall time would name the outermost scope every time
            // — a prefab always outlasts the meshes it loads — and send every investigation to the
            // container instead of to the file that spent the milliseconds.
            if ( selfMs > g_SlowestMs )
            {
                g_SlowestMs   = selfMs;
                g_SlowestPath = path;
            }
        }

        if ( !inFrame )
            return;

        // IN-FRAME LOADS NAME THEMSELVES, ALWAYS. This is the line the detector exists to print: the
        // frame the player is looking at stopped to read a file off disk.
        const uint64_t logged = g_InFrameLogged.fetch_add( 1, std::memory_order_relaxed );
        if ( logged < kInFrameLogCap )
        {
            LOG_WARN( "[SyncLoad] IN A FRAME: '{}' blocked for {} (load #{} in-frame). A load after the "
                      "first frame is a hitch the player feels — it belongs in the preload or behind a "
                      "streamer.",
                      path, Ms( totalMs ), g_InFrameLoads.load( std::memory_order_relaxed ) );
        }
        else if ( logged == kInFrameLogCap )
        {
            LOG_WARN( "[SyncLoad] {} in-frame loads logged; the rest are counted silently and reported by "
                      "SyncLoadLedger::Report().",
                      kInFrameLogCap );
        }
    }

    std::string SyncLoadLedger::Report()
    {
        std::lock_guard<std::mutex> guard( TotalsLock() );

        const uint64_t loads   = g_Loads.load( std::memory_order_relaxed );
        const uint64_t inFrame = g_InFrameLoads.load( std::memory_order_relaxed );

        std::string text = "loads=" + std::to_string( loads ) + " in " + Ms( g_TotalMs ) +
                           " (outermost scopes only; nested loads are counted, not re-timed)";
        // BOTH HALVES NAMED SEPARATELY, and zero in-frame loads is a RESULT worth printing rather than a
        // line to omit: "the preload caught everything" is the claim every later tier rests on, and a
        // report that went quiet when it was true would make the good case indistinguishable from a
        // detector that was not running.
        text += "\n  in-frame: " + std::to_string( inFrame ) + " load(s) in " + Ms( g_InFrameMs );
        if ( inFrame == 0 )
        {
            text += loads == 0 ? " — nothing loaded at all yet" : " — every load so far happened at boot";
        }
        if ( loads > 0 )
        {
            text += "\n  slowest single load by its OWN time: " + Ms( g_SlowestMs ) + " — '" + g_SlowestPath + "'";
        }
        return text;
    }

    void SyncLoadLedger::ResetForTest()
    {
        std::lock_guard<std::mutex> guard( TotalsLock() );
        g_BootFinished.store( false, std::memory_order_relaxed );
        g_Loads.store( 0, std::memory_order_relaxed );
        g_InFrameLoads.store( 0, std::memory_order_relaxed );
        g_InFrameLogged.store( 0, std::memory_order_relaxed );
        g_TotalMs   = 0.0;
        g_InFrameMs = 0.0;
        g_SlowestMs = 0.0;
        g_SlowestPath.clear();
        Depth()     = 0;
        OpenScope() = nullptr;
    }

    LoadTimingScope::LoadTimingScope( std::string path ) : m_Path( std::move( path ) )
    {
        m_Outermost = Depth() == 0;
        m_Parent    = OpenScope();
        OpenScope() = this;
        ++Depth();
        m_StartNs = NowNs();
    }

    LoadTimingScope::~LoadTimingScope()
    {
        const int64_t totalNs = NowNs() - m_StartNs;
        --Depth();
        OpenScope() = m_Parent;
        // Handed UP before this scope's own row is written, so a parent that closes later already knows
        // what its children cost. `m_ChildNs` is only ever touched by the thread that owns the stack.
        if ( m_Parent != nullptr )
        {
            m_Parent->m_ChildNs += totalNs;
        }
        SyncLoadLedger::Record( m_Path, NowMs( totalNs ), NowMs( totalNs - m_ChildNs ), m_Outermost );
    }

} // namespace Desert::Assets
