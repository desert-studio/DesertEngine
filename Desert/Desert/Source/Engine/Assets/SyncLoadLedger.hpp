#pragma once

#include <Common/Core/Logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace Desert::Assets
{
    /// WHEN a synchronous load happened, which is the whole point of the detector.
    ///
    /// `Boot` is before the first frame has begun: blocking there is normal and is what a loading screen
    /// is for. `Frame` is after it: blocking there is a hitch the player feels, and it is the failure the
    /// world programme's acceptance criterion names in its own words — "no jerks when moving". The same
    /// call, the same milliseconds, two entirely different verdicts; a counter that did not separate them
    /// would total a legitimate boot cost together with the defect and report neither.
    enum class LoadPhase : uint8_t
    {
        Boot = 0,
        Frame,
    };

    /**
     * @brief ONE POINT THROUGH WHICH EVERY SYNCHRONOUS ASSET LOAD PASSES, with a timer and a phase.
     *
     * WHY THIS IS THE HIGHEST-VALUE DETECTOR IN THE PROGRAMME, in the gap analysis's own assessment
     * (§T0.1, "highest value-per-line item"): every later claim about load time is unfalsifiable without
     * it. `MeshService::Get` parses a cooked mesh on the frame that first touches it — 40 185 992 bytes
     * of JSON for 105 317 vertices — and nothing counted that, so "the preloader is eager and that is
     * the cost" and "the preloader is lazy and the cost moved into the frame" produced the same evidence:
     * none.
     *
     * ── WHERE THE POINT IS, AND WHY IT IS NOT A CALL SITE ─────────────────────────────────────────────
     *
     * The tempting answer was `AssetManager::CreateAsset`, which is where an asset is registered and
     * eagerly loaded. It is not the point: measured on this tree, `AssetBase::Load()` is reached from
     * **45 call sites** outside the tests — hot reload has six, the component registry five, the cloud
     * panels eight, `EnsureLoaded` one, `CreateAsset` one — and a detector at any one of them counts a
     * fraction. Wrapping all 45 by hand is the arrangement `ResourceLedger`'s header refuses by name:
     * "a Register()/Unregister() pair is thirteen places for a fourteenth to be forgotten".
     *
     * So the point is `Load()` ITSELF. `AssetBase::Load()` used to be pure virtual; it is now a
     * NON-VIRTUAL member that times, counts and delegates to a protected `virtual LoadFromFile()`. Every
     * one of the 45 sites is instrumented without being edited, and a nineteenth asset type is
     * instrumented by the act of compiling: it cannot override `Load()` — the `override` keyword no
     * longer applies to it — and `Desert/Tests/Engine/SyncLoadChokepoint` asserts over the source text
     * that no subclass declares one anyway, because a declaration WITHOUT `override` would shadow
     * silently, which is the one remaining way past.
     *
     * ── NESTING, AND WHY THE TOTAL IS NOT THE SUM OF THE PARTS ───────────────────────────────────────
     *
     * Loads nest. `CloudTypeAsset::Load()` reaches into the asset manager mid-load to resolve the noise
     * volume it names, and the prefab loader loads nested prefabs. A timer that added every level's
     * duration would report a boot spending 250 % of its own wall clock. So `Loads` counts every load at
     * every depth — that is the number "how many files did we open" wants — while `TotalMs` accumulates
     * only the OUTERMOST scope, which is the number "how long did loading take" wants. They are
     * deliberately not the same question and deliberately not the same field.
     *
     * ── WHY THERE IS NO COMMAND-LINE SWITCH ──────────────────────────────────────────────────────────
     *
     * §T0.1 modelled this on Lyra's `ShouldLogAssetLoads()`, which is behind one. Ours is not, and the
     * reason is that the flag would put the interesting half behind a default of silence: a load that
     * happens IN A FRAME is the defect, and a defect that only reports itself when someone already
     * suspected it is not a detector. In-frame loads therefore log themselves at WARN, always, capped
     * per frame so that fifty thousand of them cannot drown the log they are the evidence in. Boot loads
     * are counted and totalled, and the total goes out with the memory readout; they are not individually
     * logged, because nothing is learnt from the four-thousandth line of a preload that is working.
     */
    class SyncLoadLedger final
    {
    public:
        /// Tell the ledger the BOOT IS OVER. Idempotent and one-way.
        ///
        /// CALLED BY THE LAYER, NOT BY THE RENDERER, and the distinction is the difference between a
        /// useful detector and a noisy one. The editor presents frames THROUGHOUT its staged boot — that
        /// is what the loading overlay is — so hanging the phase change off `Renderer::BeginFrame` would
        /// mark every preload stage as an in-frame load and bury the real ones under four thousand
        /// warnings. A loading screen is the boot; the phase flips when the layer says the world is
        /// playable: `EditorLayer` when its last startup stage completes, `RuntimeLayer` at the end of
        /// `OnAttach`.
        static void NoteBootFinished();

        [[nodiscard]] static LoadPhase Phase();

        /// Every load, at every nesting depth.
        [[nodiscard]] static uint64_t Loads();
        /// Wall time inside OUTERMOST loads only — see the nesting note above.
        [[nodiscard]] static double TotalMs();
        /// The subset that happened after the first frame began, and their time. This pair is the
        /// deliverable: nonzero `InFrameLoads` on a scene that has finished booting is a hitch with a
        /// name.
        [[nodiscard]] static uint64_t InFrameLoads();
        [[nodiscard]] static double   InFrameMs();
        /// The single slowest load seen BY ITS OWN TIME, and what it was. For the line that says which
        /// file to go and open.
        ///
        /// SELF TIME, NOT WALL TIME, and the first version of this got it wrong: a prefab that loads six
        /// meshes always outlasts each of them, so a wall-clock ranking names the container every time
        /// and the file that actually spent the milliseconds never appears. Exclusive time — the scope's
        /// own duration minus everything its children spent — is what a profiler reports for exactly
        /// this reason, and the suite that caught the mistake asserts the inner file wins.
        [[nodiscard]] static double      SlowestMs();
        [[nodiscard]] static std::string SlowestPath();

        /// The lines a person reads. Boot and in-frame separately, never summed.
        [[nodiscard]] static std::string Report();

        static void ResetForTest();

    private:
        friend class LoadTimingScope;
        static void Record( const std::string& path, double totalMs, double selfMs, bool outermost );
    };

    /**
     * @brief The scope that does the counting. Constructed by `AssetBase::Load()` and by nothing else.
     *
     * A TYPE RATHER THAN A PAIR OF CALLS, for the reason `ResourceOwnership` gives beside it: a load that
     * returns early — and eleven of the eighteen `LoadFromFile` bodies have at least one early return —
     * would skip the closing call, and a scope cannot. Its depth bookkeeping is what makes the nesting
     * rule above hold without any body knowing about it.
     */
    class LoadTimingScope final
    {
    public:
        explicit LoadTimingScope( std::string path );
        ~LoadTimingScope();

        LoadTimingScope( const LoadTimingScope& )            = delete;
        LoadTimingScope& operator=( const LoadTimingScope& ) = delete;
        LoadTimingScope( LoadTimingScope&& )                 = delete;
        LoadTimingScope& operator=( LoadTimingScope&& )      = delete;

    private:
        std::string m_Path;
        int64_t     m_StartNs = 0;
        /// Nanoseconds this scope's CHILDREN spent, added by each of them as it closes. Subtracting it
        /// is what turns wall time into self time; kept on the parent rather than in a side table
        /// because the parent is the only object that is certainly alive for the whole of a child.
        int64_t          m_ChildNs   = 0;
        LoadTimingScope* m_Parent    = nullptr;
        bool             m_Outermost = false;
    };

    // A NAMED DETAIL NAMESPACE AND INLINE ACCESSORS, NOT AN ANONYMOUS NAMESPACE, and
    // `Graphic/ResourceLedger.hpp` states the reason beside its own copy of this shape: "an anonymous
    // namespace in a header gives every translation unit its own copy of these statics — so the ledger
    // would count per-TU and every number it reports would be a fraction of the truth, silently.
    // `inline` in a named namespace is what gives the whole program one map."
    //
    // AND WHY THIS IS A HEADER AT ALL, WHICH IS A MEASUREMENT. It was a .cpp, and the full sweep turned
    // FIVE suites red at the link step (`AssetEviction`, `AssetPathIdentity`, `AssetHandleStability`,
    // `AssetMissingFile`, `AnimGraphAsset`): each of them compiles a handful of asset sources directly
    // rather than linking libDesert — deliberately, because libDesert pulls in Vulkan and the whole
    // renderer — and `AssetBase::Load()` is now inline in every one of them. A .cpp would mean every
    // present and future suite that touches an asset has to list this file, which is the "fourteenth
    // place to forget" the design of this detector exists to avoid.
    namespace SyncLoadDetail
    {
        inline std::atomic<bool>& BootFinished()
        {
            static std::atomic<bool> finished{ false };
            return finished;
        }

        inline std::atomic<uint64_t>& LoadCount()
        {
            static std::atomic<uint64_t> loads{ 0 };
            return loads;
        }

        inline std::atomic<uint64_t>& InFrameCount()
        {
            static std::atomic<uint64_t> loads{ 0 };
            return loads;
        }

        // THE DEPTH IS THREAD-LOCAL, the totals are not, and the split is load-bearing. The preloader
        // runs staged work and hot reload runs off the watcher, so two threads can be inside a load at
        // once; a shared depth counter would let thread B's nested load be attributed as thread A's
        // outermost one, and the "outermost only" rule that keeps TotalMs under wall clock would break in
        // a way that shows up as a boot spending 250 % of its own duration.
        inline int& Depth()
        {
            static thread_local int depth = 0;
            return depth;
        }

        /// The innermost open scope on THIS thread, so a closing scope can hand its duration to its
        /// parent. Thread-local for the same reason the depth is: the preloader runs staged work and hot
        /// reload runs off the watcher, and a shared pointer here would let one thread's load be
        /// subtracted from another thread's.
        inline LoadTimingScope*& OpenScope()
        {
            static thread_local LoadTimingScope* open = nullptr;
            return open;
        }

        inline std::mutex& TotalsLock()
        {
            static std::mutex lock;
            return lock;
        }

        /// The four figures the lock protects, in one struct so there is one static rather than four.
        struct Totals
        {
            double      TotalMs   = 0.0;
            double      InFrameMs = 0.0;
            double      SlowestMs = 0.0;
            std::string SlowestPath;
        };

        inline Totals& Sums()
        {
            static Totals totals;
            return totals;
        }

        /// How many in-frame loads may log themselves per frame before the rest are counted silently.
        ///
        /// A CAP AND NOT A SWITCH. Fifty thousand entities touching their meshes in one frame would
        /// produce fifty thousand WARN lines, and a log that cannot be read is the same as no log — but
        /// so is a log that is off by default. Sixteen is enough to name the offenders (they repeat) and
        /// few enough to read. The suppressed count still goes out, because "16 loads" and "16 logged of
        /// 4000" are different facts.
        constexpr uint64_t kInFrameLogCap = 16;

        inline std::atomic<uint64_t>& InFrameLogged()
        {
            static std::atomic<uint64_t> logged{ 0 };
            return logged;
        }

        inline double NowMs( const int64_t ns )
        {
            return static_cast<double>( ns ) / 1'000'000.0;
        }

        inline int64_t NowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch() )
                 .count();
        }

        inline std::string Ms( const double ms )
        {
            char text[32];
            std::snprintf( text, sizeof( text ), "%.2f ms", ms );
            return text;
        }
    } // namespace SyncLoadDetail

    inline void SyncLoadLedger::NoteBootFinished()
    {
        SyncLoadDetail::BootFinished().store( true, std::memory_order_relaxed );
    }

    inline LoadPhase SyncLoadLedger::Phase()
    {
        return SyncLoadDetail::BootFinished().load( std::memory_order_relaxed ) ? LoadPhase::Frame
                                                                                : LoadPhase::Boot;
    }

    inline uint64_t SyncLoadLedger::Loads()
    {
        return SyncLoadDetail::LoadCount().load( std::memory_order_relaxed );
    }

    inline uint64_t SyncLoadLedger::InFrameLoads()
    {
        return SyncLoadDetail::InFrameCount().load( std::memory_order_relaxed );
    }

    inline double SyncLoadLedger::TotalMs()
    {
        std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().TotalMs;
    }

    inline double SyncLoadLedger::InFrameMs()
    {
        std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().InFrameMs;
    }

    inline double SyncLoadLedger::SlowestMs()
    {
        std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().SlowestMs;
    }

    inline std::string SyncLoadLedger::SlowestPath()
    {
        std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().SlowestPath;
    }

    inline void SyncLoadLedger::Record( const std::string& path, const double totalMs, const double selfMs,
                                        const bool outermost )
    {
        SyncLoadDetail::LoadCount().fetch_add( 1, std::memory_order_relaxed );

        const bool inFrame = SyncLoadLedger::Phase() == LoadPhase::Frame;
        if ( inFrame )
        {
            SyncLoadDetail::InFrameCount().fetch_add( 1, std::memory_order_relaxed );
        }

        {
            std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
            if ( outermost )
            {
                SyncLoadDetail::Sums().TotalMs += totalMs;
                if ( inFrame )
                    SyncLoadDetail::Sums().InFrameMs += totalMs;
            }
            // RANKED BY SELF TIME, AT EVERY DEPTH. Wall time would name the outermost scope every time
            // — a prefab always outlasts the meshes it loads — and send every investigation to the
            // container instead of to the file that spent the milliseconds.
            if ( selfMs > SyncLoadDetail::Sums().SlowestMs )
            {
                SyncLoadDetail::Sums().SlowestMs   = selfMs;
                SyncLoadDetail::Sums().SlowestPath = path;
            }
        }

        if ( !inFrame )
            return;

        // IN-FRAME LOADS NAME THEMSELVES, ALWAYS. This is the line the detector exists to print: the
        // frame the player is looking at stopped to read a file off disk.
        const uint64_t logged = SyncLoadDetail::InFrameLogged().fetch_add( 1, std::memory_order_relaxed );
        if ( logged < SyncLoadDetail::kInFrameLogCap )
        {
            LOG_WARN( "[SyncLoad] IN A FRAME: '{}' blocked for {} (load #{} in-frame). A load after the "
                      "first frame is a hitch the player feels — it belongs in the preload or behind a "
                      "streamer.",
                      path, SyncLoadDetail::Ms( totalMs ),
                      SyncLoadDetail::InFrameCount().load( std::memory_order_relaxed ) );
        }
        else if ( logged == SyncLoadDetail::kInFrameLogCap )
        {
            LOG_WARN( "[SyncLoad] {} in-frame loads logged; the rest are counted silently and reported by "
                      "SyncLoadLedger::Report().",
                      SyncLoadDetail::kInFrameLogCap );
        }
    }

    inline std::string SyncLoadLedger::Report()
    {
        std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );

        const uint64_t loads   = SyncLoadDetail::LoadCount().load( std::memory_order_relaxed );
        const uint64_t inFrame = SyncLoadDetail::InFrameCount().load( std::memory_order_relaxed );

        std::string text = "loads=" + std::to_string( loads ) + " in " +
                           SyncLoadDetail::Ms( SyncLoadDetail::Sums().TotalMs ) +
                           " (outermost scopes only; nested loads are counted, not re-timed)";
        // BOTH HALVES NAMED SEPARATELY, and zero in-frame loads is a RESULT worth printing rather than a
        // line to omit: "the preload caught everything" is the claim every later tier rests on, and a
        // report that went quiet when it was true would make the good case indistinguishable from a
        // detector that was not running.
        text += "\n  in-frame: " + std::to_string( inFrame ) + " load(s) in " +
                SyncLoadDetail::Ms( SyncLoadDetail::Sums().InFrameMs );
        if ( inFrame == 0 )
        {
            text += loads == 0 ? " — nothing loaded at all yet" : " — every load so far happened at boot";
        }
        if ( loads > 0 )
        {
            text += "\n  slowest single load by its OWN time: " +
                    SyncLoadDetail::Ms( SyncLoadDetail::Sums().SlowestMs ) + " — '" +
                    SyncLoadDetail::Sums().SlowestPath + "'";
        }
        return text;
    }

    inline void SyncLoadLedger::ResetForTest()
    {
        std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        SyncLoadDetail::BootFinished().store( false, std::memory_order_relaxed );
        SyncLoadDetail::LoadCount().store( 0, std::memory_order_relaxed );
        SyncLoadDetail::InFrameCount().store( 0, std::memory_order_relaxed );
        SyncLoadDetail::InFrameLogged().store( 0, std::memory_order_relaxed );
        SyncLoadDetail::Sums().TotalMs   = 0.0;
        SyncLoadDetail::Sums().InFrameMs = 0.0;
        SyncLoadDetail::Sums().SlowestMs = 0.0;
        SyncLoadDetail::Sums().SlowestPath.clear();
        SyncLoadDetail::Depth()     = 0;
        SyncLoadDetail::OpenScope() = nullptr;
    }

    inline LoadTimingScope::LoadTimingScope( std::string path ) : m_Path( std::move( path ) )
    {
        m_Outermost                 = SyncLoadDetail::Depth() == 0;
        m_Parent                    = SyncLoadDetail::OpenScope();
        SyncLoadDetail::OpenScope() = this;
        ++SyncLoadDetail::Depth();
        m_StartNs = SyncLoadDetail::NowNs();
    }

    inline LoadTimingScope::~LoadTimingScope()
    {
        const int64_t totalNs = SyncLoadDetail::NowNs() - m_StartNs;
        --SyncLoadDetail::Depth();
        SyncLoadDetail::OpenScope() = m_Parent;
        // Handed UP before this scope's own row is written, so a parent that closes later already knows
        // what its children cost. `m_ChildNs` is only ever touched by the thread that owns the stack.
        if ( m_Parent != nullptr )
        {
            m_Parent->m_ChildNs += totalNs;
        }
        SyncLoadLedger::Record( m_Path, SyncLoadDetail::NowMs( totalNs ),
                                SyncLoadDetail::NowMs( totalNs - m_ChildNs ), m_Outermost );
    }

} // namespace Desert::Assets
