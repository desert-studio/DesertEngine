#pragma once

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

        /// HOW MUCH OF THE READING STOPPED BLOCKING ANYTHING. A load that ran on a `JobSystem` worker
        /// under an `AsyncLoadMarker` is counted here and is deliberately NOT counted as in-frame, even
        /// when it happened long after the boot: `InFrameLoads()` answers "did a frame stop to read a
        /// file", and a worker thread reading while the frame draws is the opposite of that. Counting it
        /// there would have made this whole tier's success look identical to its failure — the moment
        /// the cloud kinds moved off the boot, the detector would have gone from 0 in-frame loads to
        /// fifteen and reported the fix as the defect.
        ///
        /// Still inside `Loads()`, because that number is "how many files were read" and the thread it
        /// happened on does not change the answer.
        [[nodiscard]] static uint64_t AsyncLoads();
        [[nodiscard]] static double   AsyncMs();
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

        // THE STATE IS THE INSTRUMENT TOO, so it goes with the bodies. Left standing under Shipping it is
        // 40 bytes per asset load that nothing reads — and clang says so, five times per build
        // (-Wunused-private-field), which is a warning the tree is at zero for and must stay at zero for.
    private:
#if DESERT_DEV_INSTRUMENTS
        std::string m_Path;
        int64_t     m_StartNs = 0;
        /// Nanoseconds this scope's CHILDREN spent, added by each of them as it closes. Subtracting it
        /// is what turns wall time into self time; kept on the parent rather than in a side table
        /// because the parent is the only object that is certainly alive for the whole of a child.
        int64_t          m_ChildNs   = 0;
        LoadTimingScope* m_Parent    = nullptr;
        bool             m_Outermost = false;
#endif
    };

    /**
     * @brief Marks the calling thread as being inside an ASYNCHRONOUS read, for as long as it lives.
     *
     * WHY THE DETECTOR NEEDS TO BE TOLD. `SyncLoadLedger` splits every load into "at boot" and "in a
     * frame", and the second one is a warning because a frame that stops to read a file is a hitch the
     * player feels. That split has exactly one blind spot, and `AsyncAssetLoader` is built on it: a
     * `JobSystem` worker reading a `.dcnv` while the main thread draws is *after* the boot and *not* in
     * the frame. Without this marker every such read would be logged as the very defect this tier was
     * built to remove, and the fix would be indistinguishable from the disease in the one instrument
     * that measures it.
     *
     * THREAD-LOCAL, like the depth beside it and for the same reason: one worker being async says
     * nothing about what the main thread is doing at the same moment.
     */
    class AsyncLoadMarker final
    {
    public:
        AsyncLoadMarker();
        ~AsyncLoadMarker();

        AsyncLoadMarker( const AsyncLoadMarker& )            = delete;
        AsyncLoadMarker& operator=( const AsyncLoadMarker& ) = delete;
        AsyncLoadMarker( AsyncLoadMarker&& )                 = delete;
        AsyncLoadMarker& operator=( AsyncLoadMarker&& )      = delete;
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

        inline std::atomic<uint64_t>& AsyncCount()
        {
            static std::atomic<uint64_t> loads{ 0 };
            return loads;
        }

        /// How many `AsyncLoadMarker`s are open on THIS thread. A counter rather than a flag because a
        /// completion delegate is allowed to request the next asset, and nesting must not clear the
        /// outer mark on the way out of the inner one.
        inline int& AsyncDepth()
        {
            static thread_local int depth = 0;
            return depth;
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
            double      AsyncMs   = 0.0;
            double      SlowestMs = 0.0;
            std::string SlowestPath;
            /// The slowest in-frame loads by their OWN time, slowest first, at most `kInFrameRankCap`.
            std::vector<std::pair<double, std::string>> SlowestInFrame;
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

        /// How many in-frame loads `Report()` names, ranked by their own time.
        ///
        /// THE LOG CAP KEEPS THE FIRST SIXTEEN, THIS KEEPS THE WORST SIXTEEN. A frame that stalls on a
        /// 300 ms bake after two hundred cheap texture reads would otherwise be reported as "the rest are
        /// counted silently" — the one load worth opening would sit in the unnamed tail. Ranking costs a
        /// bounded insert under a lock the load already takes, so it runs for every in-frame load.
        constexpr std::size_t kInFrameRankCap = 16;

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
        const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().TotalMs;
    }

    inline double SyncLoadLedger::InFrameMs()
    {
        const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().InFrameMs;
    }

    inline uint64_t SyncLoadLedger::AsyncLoads()
    {
        return SyncLoadDetail::AsyncCount().load( std::memory_order_relaxed );
    }

    inline double SyncLoadLedger::AsyncMs()
    {
        const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().AsyncMs;
    }

    inline double SyncLoadLedger::SlowestMs()
    {
        const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().SlowestMs;
    }

    inline std::string SyncLoadLedger::SlowestPath()
    {
        const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        return SyncLoadDetail::Sums().SlowestPath;
    }

    inline void SyncLoadLedger::Record( const std::string& path, const double totalMs, const double selfMs,
                                        const bool outermost )
    {
        SyncLoadDetail::LoadCount().fetch_add( 1, std::memory_order_relaxed );

        // AN ASYNC READ IS NOT AN IN-FRAME READ, and this line is the whole of that distinction. A
        // worker inside `AsyncAssetLoader` is reading while the frame draws rather than instead of it,
        // so it is counted, timed and reported — but never as the hitch the warning below names.
        const bool async = SyncLoadDetail::AsyncDepth() > 0;
        if ( async )
        {
            SyncLoadDetail::AsyncCount().fetch_add( 1, std::memory_order_relaxed );
        }

        const bool inFrame = !async && SyncLoadLedger::Phase() == LoadPhase::Frame;
        if ( inFrame )
        {
            SyncLoadDetail::InFrameCount().fetch_add( 1, std::memory_order_relaxed );
        }

        {
            const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
            if ( outermost )
            {
                SyncLoadDetail::Sums().TotalMs += totalMs;
                if ( inFrame )
                    SyncLoadDetail::Sums().InFrameMs += totalMs;
                if ( async )
                    SyncLoadDetail::Sums().AsyncMs += totalMs;
            }
            // RANKED BY SELF TIME, AT EVERY DEPTH. Wall time would name the outermost scope every time
            // — a prefab always outlasts the meshes it loads — and send every investigation to the
            // container instead of to the file that spent the milliseconds.
            if ( selfMs > SyncLoadDetail::Sums().SlowestMs )
            {
                SyncLoadDetail::Sums().SlowestMs   = selfMs;
                SyncLoadDetail::Sums().SlowestPath = path;
            }
            if ( inFrame )
            {
                auto& ranked = SyncLoadDetail::Sums().SlowestInFrame;
                if ( ranked.size() < SyncLoadDetail::kInFrameRankCap || selfMs > ranked.back().first )
                {
                    const auto slot = std::upper_bound(
                         ranked.begin(), ranked.end(), selfMs,
                         []( const double ms, const std::pair<double, std::string>& entry )
                         { return ms > entry.first; } );
                    ranked.insert( slot, { selfMs, path } );
                    if ( ranked.size() > SyncLoadDetail::kInFrameRankCap )
                        ranked.pop_back();
                }
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
            LOG_WARN( "[SyncLoad] {} in-frame loads logged; the rest are not logged one by one — "
                      "SyncLoadLedger::Report() names the {} slowest of all of them by their own time.",
                      SyncLoadDetail::kInFrameLogCap, SyncLoadDetail::kInFrameRankCap );
        }
    }

    inline std::string SyncLoadLedger::Report()
    {
        const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );

        const uint64_t loads   = SyncLoadDetail::LoadCount().load( std::memory_order_relaxed );
        const uint64_t inFrame = SyncLoadDetail::InFrameCount().load( std::memory_order_relaxed );
        const uint64_t async   = SyncLoadDetail::AsyncCount().load( std::memory_order_relaxed );

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
        const auto& ranked = SyncLoadDetail::Sums().SlowestInFrame;
        if ( !ranked.empty() )
        {
            text += "\n  slowest in-frame load(s) by their OWN time (" + std::to_string( ranked.size() ) +
                    " of " + std::to_string( inFrame ) + "):";
            for ( const auto& [ms, rankedPath] : ranked )
                text += "\n    " + SyncLoadDetail::Ms( ms ) + " — '" + rankedPath + "'";
            // The unnamed remainder is BOUNDED rather than merely counted: every one of them took no
            // longer than the last named load, which is what decides whether they are worth chasing.
            if ( inFrame > ranked.size() )
                text += "\n    the other " + std::to_string( inFrame - ranked.size() ) +
                        " took at most " + SyncLoadDetail::Ms( ranked.back().first ) + " each";
        }
        // NAMED WHETHER OR NOT IT IS ZERO, for the reason the in-frame line is: "none of this boot's
        // reading was asynchronous" is a fact about the model, and a line that appeared only once the
        // number was interesting would make the eager case look like a missing detector.
        text += "\n  async (on a JobSystem worker, blocking no frame): " + std::to_string( async ) +
                " load(s) in " + SyncLoadDetail::Ms( SyncLoadDetail::Sums().AsyncMs );
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
        const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );
        SyncLoadDetail::BootFinished().store( false, std::memory_order_relaxed );
        SyncLoadDetail::LoadCount().store( 0, std::memory_order_relaxed );
        SyncLoadDetail::InFrameCount().store( 0, std::memory_order_relaxed );
        SyncLoadDetail::AsyncCount().store( 0, std::memory_order_relaxed );
        SyncLoadDetail::InFrameLogged().store( 0, std::memory_order_relaxed );
        SyncLoadDetail::Sums().TotalMs   = 0.0;
        SyncLoadDetail::Sums().InFrameMs = 0.0;
        SyncLoadDetail::Sums().AsyncMs   = 0.0;
        SyncLoadDetail::Sums().SlowestMs = 0.0;
        SyncLoadDetail::Sums().SlowestPath.clear();
        SyncLoadDetail::Sums().SlowestInFrame.clear();
        SyncLoadDetail::Depth()      = 0;
        SyncLoadDetail::AsyncDepth() = 0;
        SyncLoadDetail::OpenScope()  = nullptr;
    }

    // THE THREE ASSIGNMENTS BELOW CANNOT BECOME MEMBER INITIALISERS, and the reason is ORDER.
    // Member initialisers run in DECLARATION order, and this constructor has two side effects that must
    // happen between them: the thread's open-scope pointer may only be re-pointed at `this` AFTER
    // `m_Parent` has read its old value, and the clock must be read LAST so that none of the bookkeeping
    // is charged to the load being timed. A member initialiser list cannot interleave statements, so
    // obeying the check here would either lose the parent link or start the timer before the bookkeeping.
    // ── THE SHIPPING BOUNDARY, AND IT IS ON THE TWO SCOPES ONLY ─────────────────────────────────────
    //
    // Everything else in this header is an `inline` function in a named namespace, so it costs a shipping
    // binary nothing on its own: an inline definition with no caller is never emitted. The whole ledger
    // reaches the player through exactly two objects — `LoadTimingScope`, constructed by `AssetBase::Load`
    // on EVERY asset read, and `AsyncLoadMarker` on every worker read — and emptying those two is what
    // removes the clock read, the thread-local stack and the locked totals from the player's load path.
    //
    // The declarations and the signatures are untouched on purpose. `Desert/Tests/Engine/
    // SyncLoadChokepoint` asserts over the SOURCE TEXT of `AssetBase.hpp` that `Load()` still takes a
    // `LoadTimingScope`; a boundary drawn at that call site would have read to the census as the
    // chokepoint being removed. Here the call site is identical in every configuration and only the body
    // differs.
    //
    // ONE RESIDUAL, NAMED RATHER THAN HIDDEN: the argument is still built by the caller
    // (`m_Metadata.Filepath.string()`), so a shipping build still pays one std::string per asset load.
    // Removing that means changing the call site, which is the census's text — a separate change with a
    // separate argument to make.
#if !DESERT_DEV_INSTRUMENTS

    inline LoadTimingScope::LoadTimingScope( std::string )
    {
    }
    inline LoadTimingScope::~LoadTimingScope() = default;
    inline AsyncLoadMarker::AsyncLoadMarker()
    {
    }
    inline AsyncLoadMarker::~AsyncLoadMarker() = default;

#else

    // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
    inline LoadTimingScope::LoadTimingScope( std::string path ) : m_Path( std::move( path ) )
    {
        m_Outermost                 = SyncLoadDetail::Depth() == 0;
        m_Parent                    = SyncLoadDetail::OpenScope();
        SyncLoadDetail::OpenScope() = this;
        ++SyncLoadDetail::Depth();
        m_StartNs = SyncLoadDetail::NowNs();
    }
    // NOLINTEND(cppcoreguidelines-prefer-member-initializer)

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

        // A DETECTOR MUST NEVER BE THE REASON THE PROGRAM DIES. `Record` builds strings — the path, the
        // report line — so it can throw `std::bad_alloc`, and an exception leaving a destructor calls
        // `std::terminate`. That would turn "the machine is short of memory", which is EXACTLY the
        // condition this instrument exists to observe, into a crash with the measurement lost. The catch
        // is deliberately empty because there is nothing safe left to do: logging allocates too.
        try
        {
            SyncLoadLedger::Record( m_Path, SyncLoadDetail::NowMs( totalNs ),
                                    SyncLoadDetail::NowMs( totalNs - m_ChildNs ), m_Outermost );
        }
        catch ( ... ) // NOLINT(bugprone-empty-catch) -- see above: there is no safe action left
        {
        }
    }

    inline AsyncLoadMarker::AsyncLoadMarker()
    {
        ++SyncLoadDetail::AsyncDepth();
    }

    inline AsyncLoadMarker::~AsyncLoadMarker()
    {
        --SyncLoadDetail::AsyncDepth();
    }

#endif // DESERT_DEV_INSTRUMENTS
} // namespace Desert::Assets
