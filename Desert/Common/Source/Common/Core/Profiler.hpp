#pragma once

// DesertEngine CPU profiler.
//
// Two layers behind one set of macros:
//   1. Optick — every scope also emits an OPTICK_EVENT, so the external Optick GUI (connect over the
//      socket server) gets the full timeline. Frame boundaries flip Optick's frame.
//   2. A lightweight in-engine aggregator (this file) — accumulates named scopes per frame so the editor
//      can show a readable "Profiler" panel and dump timings to the log WITHOUT the external GUI. This is
//      what we use to localize FPS hotspots directly.
//
// Use DESERT_PROFILE_FRAME(name) once at the top of the frame loop, and DESERT_PROFILE_SCOPE(name) inside
// any scope you want timed. Names must be string literals (so Optick can cache its descriptors).

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/GpuQueue.hpp>
#include <optick.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Common::Profiling
{
    // THE INTERNED SCOPE NAME (UE: a stat's FName / TStatId). Every row of the profiler is keyed by a small id
    // the name table hands out once per distinct name; a closing scope adds to its row BY ID, so no scope end
    // hashes, compares or allocates a string. Text is the table's copy of the name: the table never frees or
    // moves one, so it is valid for the process' life (Optick's dynamic events and the published rows read it).
    inline constexpr uint32_t kNoScopeName = UINT32_MAX;
    struct InternedScopeName
    {
        uint32_t    Id   = kNoScopeName;
        const char* Text = nullptr;
    };

    namespace Detail
    {
        // One thread's sample buffer (Profiler.cpp; UE FThreadStats). Owned by the profiler, written by its
        // thread alone, read by the publishing thread only on the half its thread has stopped writing.
        struct ThreadStats;
        // The window being accumulated, drained from every thread once a frame (Profiler.cpp).
        struct WindowState;
    } // namespace Detail

    struct ScopeResult
    {
        std::string Name;
        double      TotalMs = 0.0; // summed across all calls this frame; INCLUSIVE of every nested scope
        uint32_t    Calls   = 0;
        // TotalMs with every nested CPU scope subtracted: what ran in this scope's own lines. Unlike TotalMs it
        // can be summed across rows (on one thread).
        double SelfMs = 0.0;
        // The part of TotalMs this thread spent BLOCKED in a ScopeKind::Wait scope (its own, or one nested in
        // it at any depth): a fence, an acquire, a present, the frame pacer. TotalMs - WaitMs is the scope's
        // work. UE keeps waits as stats of their own for the same reason: a present scope that "costs 8 ms" is
        // almost always the GPU or the display being waited for, and an inclusive number cannot say which.
        double WaitMs = 0.0;
        // GPU time for the SAME name, measured by the backend's timestamp queries. Zero for a scope that
        // records no GPU work (most of them) and for a build/device without timestamp support. It arrives
        // MaxFramesInFlight frames late (see IGpuProfilerSink) — invisible here because both numbers are
        // averaged over the same window, which is much longer than that lag.
        double   GpuMs    = 0.0;
        uint32_t GpuCalls = 0;
        // GPU time with every nested pass subtracted out. GpuMs answers "what does this pass cost me if I
        // delete it, children and all"; GpuSelfMs answers "what happened HERE". Only the self column can be
        // summed: the inclusive one counts a parent's microseconds again in every child, which is why a
        // naive total came to 159% of the frame the first time this was run.
        double GpuSelfMs = 0.0;
        // The row's CPU ms (inclusive, all calls) in its costliest frame of the window, and the 99th percentile
        // over the window's frames (a frame the row did not run in costs 0). The average hides a spike; these
        // are what a hitch is read by.
        double MaxMs = 0.0;
        double P99Ms = 0.0;
    };

    // A HITCH: a frame of the published window whose wall time is over kHitchFactor x the window's median, with
    // the rows that spent the most SELF time in it, any thread (the dump's [Hitch] lines).
    inline constexpr uint32_t    kHitchTopScopes      = 5;
    inline constexpr double      kHitchFactor         = 1.5;
    inline constexpr std::size_t kMaxPublishedHitches = 8; // the slowest ones; LastHitchCount() counts them all
    struct HitchFrame
    {
        double                                      FrameMs  = 0.0;
        double                                      MedianMs = 0.0;
        uint32_t                                    Frame    = 0; // its index in the window
        std::vector<std::pair<std::string, double>> Top;          // row, self ms; largest first
    };

    // The row the backend publishes the whole-frame GPU bracket under. It is a DENOMINATOR, not a pass:
    // anything summing the per-pass column has to leave it out. Declared here, beside the results it
    // appears in, so the backend that writes it and the panel that reads it share one string.
    inline constexpr const char* kGpuFrameTotalScope = "GPU: Frame Total";

    // THE MESH DRAW SPLIT (UE `ProfileGPU` with `r.EmitMeshDrawEvents 1`): under Profiler::MeshDrawSplit a mesh
    // pass brackets each of its draw groups - a run of CPU draws of one mesh x one material, one GPU-driven bin -
    // with its own timestamp pair, so a pass row that costs 14 ms can say WHICH meshes cost it. The groups are not
    // scopes of the pass tree: they live in a pool of their own (VulkanGpuProfiler), never become children of
    // their pass, and leave its gpu / gpu self columns exactly as they are without the split - the split reads
    // against its pass row, it does not re-partition it.
    //
    // What a group drew, counted on the CPU while it recorded. Draws: the draw calls (one per drawn submesh of a
    // CPU draw, one per bin record). Triangles: the triangles those draws name times their instances; for a draw
    // whose instance count only the GPU knows (a GPU-culled bin, a GPU-generated instanced draw) the triangles of
    // ONE instance, with TrianglesPerInstance set - the CPU never reads the culled count back.
    struct DrawSplitCounts
    {
        uint32_t Draws                = 0;
        uint64_t Triangles            = 0;
        bool     TrianglesPerInstance = false;
    };

    // One draw group's averaged row of the published window (Profiler::LastDrawSplit): Pass is the pass ROW the
    // group recorded inside (view-prefixed like the pass's own row, GpuViewScopeName), so the two read side by
    // side; GpuMs / Draws / Triangles are per frame, averaged like every other column.
    struct DrawSplitResult
    {
        std::string Pass;
        std::string Mesh;
        std::string Material;
        double      GpuMs                = 0.0;
        double      Draws                = 0.0;
        double      Triangles            = 0.0;
        bool        TrianglesPerInstance = false;
    };

    // The prefix of the row the graph backend publishes one async compute segment under ("RDG Async: <first
    // pass> .. <last pass> (<n> passes)"): the run of passes recorded on the compute queue, overlapping the
    // graphics queue. Its time is NOT on the graphics timeline, so a reader adding self times up against the
    // frame bracket has to tell these rows apart; the backend that writes it and the dump that reads it share it.
    inline constexpr const char* kGpuAsyncSegmentScopePrefix = "RDG Async: ";

    // Where an async segment row's label starts in @p name: after "RDG Async: ", or after
    // "<view> | RDG Async: " for a segment recorded inside a view (GpuViewScopeName puts the view's name first).
    // npos: not an async segment row. The backend's per-queue remainder and the dump's async line both ask it.
    [[nodiscard]] inline size_t GpuAsyncSegmentLabelStart( std::string_view name )
    {
        const std::string_view prefix = kGpuAsyncSegmentScopePrefix;
        if ( name.starts_with( prefix ) )
            return prefix.size();
        const size_t viewEnd = name.find( " | " );
        if ( viewEnd != std::string_view::npos && name.substr( viewEnd + 3 ).starts_with( prefix ) )
            return viewEnd + 3 + prefix.size();
        return std::string_view::npos;
    }

    // UNACCOUNTED, PER QUEUE (UE "stat gpu": the Unaccounted row). The backend publishes, every frame:
    //   - graphics: the GPU frame bracket minus the UNION of the graphics-queue scopes inside it - device work
    //     (barriers, copies, uploads, work recorded outside every pass) and device idle no scope is around;
    //   - async compute: the async segments minus the union of the async passes inside them.
    // A union, not a sum of self times: a scope of one view open around another view's passes, or an async
    // pass nested under a graphics scope, would be counted twice by a sum and makes the remainder a lie.
    // These rows are REMAINDERS, not passes: anything summing the per-pass column leaves them out.
    inline constexpr const char* kGpuUnaccountedGraphicsScope = "GPU Unaccounted: graphics queue";
    inline constexpr const char* kGpuUnaccountedAsyncScope    = "GPU Unaccounted: async compute queue";
    // Where the graphics remainder sits: one row per uncovered stretch of the graphics timeline of at least
    // kGpuGapReportMs, named by the scope that ended last before it and the one that began after it
    // ("GPU Gap: <after> -> <before>", "frame begin" / "frame end" at the bracket's edges). The next
    // measurement names the unlabelled work by its neighbours instead of leaving a number to guess at.
    inline constexpr const char* kGpuGapScopePrefix = "GPU Gap: ";
    inline constexpr double      kGpuGapReportMs    = 0.1;

    // What a CPU scope is. Work is the default; Wait marks a scope whose time is the thread BLOCKED on
    // something outside it (vkWaitForFences, vkAcquireNextImageKHR, vkQueuePresentKHR, the frame pacer's
    // sleep). Every enclosing scope reports that time in its WaitMs column, so a parent row splits into work
    // and wait without a second name for either half.
    enum class ScopeKind : uint8_t
    {
        Work,
        Wait,
    };

    // How the backend (Vulkan) is told where a GPU scope starts and ends. Common must not know about
    // Vulkan, so the graphics layer installs an implementation and the profiling macros call through it.
    //
    // BeginScope returns an opaque handle, or -1 when the scope cannot be timed this frame (no command
    // buffer recording, GPU timing off, the frame's query budget spent). EndScope ignores -1, so a caller
    // never has to check.
    class IGpuProfilerSink
    {
    public:
        virtual ~IGpuProfilerSink() = default;

        virtual int32_t BeginScope( const char* name ) = 0;
        virtual void    EndScope( int32_t handle )     = 0;
        // The queue the scopes opened from now on are recorded on. The render graph's backend switches it at
        // every pipe segment it begins and ends; outside the graph it is Graphics (the frame's own buffer).
        virtual void SetQueue( GpuQueue queue ) = 0;

        // One draw group of the mesh draw split (DrawSplitCounts): its own timestamp pair, outside the scope tree.
        // @p pass is the recording pass's name (the view prefix is the sink's, as BeginScope's), @p mesh and
        // @p material the group's labels; all three are copied. -1 when it cannot be timed (as BeginScope).
        virtual int32_t BeginDrawScope( std::string_view pass, std::string_view mesh, std::string_view material ) = 0;
        virtual void    EndDrawScope( int32_t handle, const DrawSplitCounts& counts ) = 0;
    };

    // THE CPU SCOPE AGGREGATOR, built the way UE's stats system is (FThreadStats -> the stats thread's frame):
    //   - a scope's name is interned ONCE (InternedScopeName): a DESERT_PROFILE_SCOPE call site holds its id in a
    //     static, a runtime name finds it in its thread's own name cache (LocalScopeName) - no lock either way;
    //   - a closing scope adds to ITS THREAD's buffer (Detail::ThreadStats), never to a shared map: no mutex, no
    //     string, and the render-graph workers recording in parallel never serialise on one lock;
    //   - the buffers are merged only when a window is PUBLISHED (BeginFrame): the publisher flips each thread's
    //     half and reads the half that thread has stopped writing.
    // Every thread may open scopes; the thread calling BeginFrame is the frame thread (LastFrameNamedMs).
    class Profiler
    {
    public:
        static Profiler& Get();

        Profiler();
        ~Profiler();
        Profiler( const Profiler& )            = delete;
        Profiler& operator=( const Profiler& ) = delete;

        // Per-frame boundary. Accumulates scope times across all frames in the current averaging window;
        // when the window (AvgWindowSeconds) elapses it publishes the AVERAGE per-frame time of each scope.
        // Averaging makes the numbers readable at high FPS (per-frame values flicker too fast to read).
        void BeginFrame();

        // THE NAME TABLE. @p name's id, interned on its first sight under the table's lock: what a call site's
        // static calls once (DESERT_PROFILE_SCOPE). The same characters always get the same id, so two call
        // sites, a CPU and a GPU sample of one name share one row, as they did when rows were keyed by string.
        [[nodiscard]] InternedScopeName InternScopeName( std::string_view name );
        // The id of the name @p prefix + @p name, for a RUNTIME name (a render-graph pass, a backend row): found
        // in the calling thread's own cache by the two parts' characters, so neither the concatenation is built
        // nor a lock taken once this thread has seen the name; the first sight interns it (InternScopeName).
        [[nodiscard]] InternedScopeName LocalScopeName( std::string_view prefix, std::string_view name );
        // The text of name id @p id (the table's copy, valid for the process' life); "" for no such id. Takes the
        // table's lock: for a reader that kept only ids (the GPU profiler's [GpuOrder]
        // trace), never per sample.
        [[nodiscard]] const char* ScopeNameText( uint32_t id );
#if USE_OPTICK
        // THE OPTICK EVENT OF AN INTERNED NAME, made once per name and cached in the calling thread, so a hit
        // takes no lock. OPTICK_EVENT_DYNAMIC made one per CALL (EventDescription::CreateShared, under Optick's
        // global lock) - once per render-graph pass per frame for DESERT_PROFILE_SCOPE_PREFIXED.
        [[nodiscard]] ::Optick::EventDescription* OptickEventOf( const InternedScopeName& name );
#endif

        // One closed CPU scope of row @p nameId (InternedScopeName::Id): @p totalMs inclusive, @p selfMs with
        // nested scopes subtracted, @p waitMs the part of totalMs spent in Wait scopes (ScopeResult).
        // @p outermost: no scope of the same thread was open around it - on the frame thread such a scope adds
        // to LastFrameNamedMs. Into the calling thread's buffer: no lock, no allocation, never throws (it runs in
        // ScopedTimer's destructor). Only into a row ReserveSample made on this thread; another adds nothing.
        void AddSample( uint32_t nameId, double totalMs, double selfMs, double waitMs, bool outermost ) noexcept;
        // Makes row @p nameId exist in the calling thread's buffer (the buffer too, on the thread's first scope),
        // so AddSample into it never allocates: what a ScopedTimer does when it opens. False - nothing will be
        // measured - when the profiler is off, @p nameId is kNoScopeName or the thread's buffer is retired.
        [[nodiscard]] bool ReserveSample( uint32_t nameId );

        // Resolved GPU time for a scope, fed by the backend once the queries for a finished frame are
        // readable. Lands in the same row as the CPU sample of the same name. @p inclusiveMs covers nested
        // passes, @p selfMs has them subtracted. Into the calling thread's buffer, like AddSample.
        void AddGpuSample( const char* name, double inclusiveMs, double selfMs );
        // The same into row @p nameId (InternedScopeName::Id): what a backend that keeps ids passes, no lookup.
        void AddGpuSample( uint32_t nameId, double inclusiveMs, double selfMs );

        // Installed once by the graphics backend; null in a build with no GPU timing (or a device whose
        // queues report no valid timestamp bits). Not owned.
        void SetGpuSink( IGpuProfilerSink* sink )
        {
            m_GpuSink = sink;
        }
        IGpuProfilerSink* GetGpuSink() const
        {
            return m_GpuSink;
        }

        // GPU timestamps cost real time on the GPU TIMELINE — measured at ~8 % of a debug frame on
        // MoltenVK, where a timestamp becomes a Metal counter sample that can split an encoder.
        //
        // DEFAULT OFF, deliberately. An always-on instrument that inflates the thing it measures by 8 %
        // means every performance number taken afterwards carries the tax, and sooner or later somebody
        // compares an instrumented number with an uninstrumented one. That is the defect shape this
        // engine has been burned by repeatedly: two quantities that must agree, and nothing checking.
        // An ordinary frame is therefore the shipped frame, and measuring is a deliberate act —
        // `--gpu-profile` on the command line, or the panel's GPU checkbox.
        bool& GpuEnabled()
        {
            return m_GpuEnabled;
        }

        // Per-PASS timestamps, as opposed to the single pair bracketing the whole command buffer. Turning
        // this off leaves the frame total (two timestamps, negligible) and drops the ~40 pass marks — a
        // cheap way to watch GPU frame time, and the control that lets the per-pass marks be priced
        // against the frame bracket rather than against nothing.
        bool& GpuPassScopes()
        {
            return m_GpuPassScopes;
        }

        // THE MESH DRAW SPLIT (DrawSplitCounts; UE r.EmitMeshDrawEvents): a timestamp pair per draw group of the
        // mesh passes, on top of GpuEnabled. DEFAULT OFF, and off is not "cheap": no pool, no reset, not one query
        // - a timestamp per draw group would tax the passes it measures, which is the tax GpuEnabled already
        // refuses to make the default. Switched by the palette's "Profiler.MeshDrawSplit 1 / 0".
        bool& MeshDrawSplit()
        {
            return m_MeshDrawSplit;
        }
        // `--gpu-order-trace`: the GPU profiler logs the [GpuOrder] pass-order text when the order's hash
        // changes. Off, it keeps only the hash (a line per changed frame was text built every frame).
        bool& GpuOrderTrace()
        {
            return m_GpuOrderTrace;
        }
        // Whether a mesh pass recording now should bracket its draw groups: the split asked for, GPU timing on,
        // and a sink installed. The call sites read this once per pass and build no label when it is false.
        [[nodiscard]] bool DrawSplitActive() const
        {
            return m_MeshDrawSplit && m_GpuEnabled && m_GpuSink != nullptr;
        }
        // One draw group of one resolved frame (the backend, at the same point as AddGpuSample).
        void AddDrawSplitSample( std::string_view pass, std::string_view mesh, std::string_view material,
                                 double gpuMs, const DrawSplitCounts& counts );
        // The published window's draw groups, averaged per frame like LastFrame(): sorted by Pass, then by GpuMs
        // (most expensive first). Empty when the split was off for the whole window.
        const std::vector<DrawSplitResult>& LastDrawSplit() const
        {
            return m_DrawSplitDisplay;
        }

        // Published, averaged-over-the-window results (see BeginFrame).
        const std::vector<ScopeResult>& LastFrame() const
        {
            return m_Display;
        }
        // The published window's hitches, slowest first, at most kMaxPublishedHitches (HitchFrame).
        const std::vector<HitchFrame>& LastHitches() const
        {
            return m_DisplayHitches;
        }
        [[nodiscard]] uint32_t LastHitchCount() const
        {
            return m_DisplayHitchCount;
        }
        [[nodiscard]] double LastFrameMedianMs() const
        {
            return m_DisplayMedianMs;
        }
        // How many windows have been published (UE: the stats thread's condensed frame number). LastFrame and
        // every Last* answer change only when this does, so a reader that formats them (the Profiler window)
        // formats once per publication, not once per paint.
        [[nodiscard]] uint64_t PublishCount() const
        {
            return m_PublishCount;
        }
        double LastFrameMs() const
        {
            return m_DisplayFrameMs;
        }
        // THE CPU REMAINDER'S COMPLEMENT (UE "stat unit": the frame is the sum of named stages plus what no
        // stage holds). The averaged per-frame wall time the FRAME THREAD (the one calling BeginFrame) spent
        // inside an outermost scope of its own. LastFrameMs minus this is frame-thread time no row names:
        // the CPU Unaccounted. Outermost only, so a nested row is never counted twice; worker-thread scopes
        // stay out, because they overlap the frame thread and cannot be subtracted from its wall time.
        double LastFrameNamedMs() const
        {
            return m_DisplayFrameNamedMs;
        }

        bool& Enabled()
        {
            return m_Enabled;
        }
        // When true the published list is sorted by time (desc) — handy to spot hotspots, but rows jump
        // around frame-to-frame. When false it's sorted by NAME (stable order, easy to read a single row).
        bool& SortByTime()
        {
            return m_SortByTime;
        }
        // Averaging window in seconds: values are averaged over this period and refreshed once per period.
        float& AvgWindowSeconds()
        {
            return m_AvgWindowSec;
        }
        // CLOUD-MARCH-COST-4: WHICH frames the published rows are. The window is a wall-clock bucket that
        // closes once per period, so a reader (a profile dump) gets the last CLOSED one: on a camera that
        // turns 1 degree a frame, its frame count and its age say which arc of the turn the rows measured.
        [[nodiscard]] uint32_t LastWindowFrames() const
        {
            return m_DisplayWindowFrames;
        }
        [[nodiscard]] double LastWindowAgeMs() const
        {
            return std::chrono::duration<double, std::milli>( Clock::now() - m_DisplayClosedAt ).count();
        }
        // A FRAME-EXACT CAPTURE (UE ProfileGPU over a fixed frame span). BeginCapture closes the running window
        // on the next BeginFrame and stops the time-based closing; EndCapture closes the captured window on the
        // next BeginFrame and holds it as the published rows until ReleaseCapture (the dump that read them) or
        // the next BeginCapture. The scenario `frames N` wait brackets one (ControlService), so a dump after it
        // averages exactly those frames: a full 360-frame turn, not whichever 0.5 s arc closed last.
        void BeginCapture();
        void EndCapture();
        void ReleaseCapture();
        [[nodiscard]] bool LastWindowCaptured() const
        {
            return m_DisplayCaptured;
        }

    private:
        using Clock = std::chrono::high_resolution_clock;

        // The calling thread's buffer, registered on its first sample; null once the thread is exiting (its
        // buffer retired, see Profiler.cpp), when a sample has nowhere left to go.
        Detail::ThreadStats* LocalStats();

        // THE NAME TABLE (InternScopeName). A deque, so a name's characters never move: m_NameIds' keys and
        // every InternedScopeName::Text point into it for the process' life.
        std::mutex                                     m_NameMutex;
        std::deque<std::string>                        m_NameText; // by id
        std::unordered_map<std::string_view, uint32_t> m_NameIds;
#if USE_OPTICK
        std::vector<::Optick::EventDescription*> m_OptickEvents; // by name id, under m_NameMutex
#endif

        // THE THREAD BUFFERS: every thread that ever closed a scope, until its exit retires its buffer and the
        // next publication merges what it left and drops it.
        std::mutex                                        m_ThreadsMutex;
        std::vector<std::unique_ptr<Detail::ThreadStats>> m_Threads;
        std::unique_ptr<Detail::WindowState>              m_Window; // the frame thread's (BeginFrame)
        std::vector<HitchFrame>                           m_DisplayHitches;
        uint32_t                                          m_DisplayHitchCount = 0;
        double                                            m_DisplayMedianMs   = 0.0;

        std::mutex                                       m_Mutex; // the mesh draw split's accumulation
        std::vector<ScopeResult>                     m_Display;
        uint64_t                                         m_PublishCount        = 0;
        double                                       m_DisplayFrameMs = 0.0;
        double                                           m_DisplayFrameNamedMs = 0.0; // see LastFrameNamedMs
        Clock::time_point                            m_WindowStart;
        uint32_t                                     m_WindowFrames  = 0;
        uint32_t                                     m_DisplayWindowFrames = 0; // the published window's
        Clock::time_point                            m_DisplayClosedAt{};       // ... and when it closed
        bool                                         m_WindowStarted = false;
        enum class CaptureState : uint8_t
        {
            None,
            Arming,    // close the running window on the next BeginFrame, then Capturing
            Capturing, // no time-based close
            Ending,    // close the captured window on the next BeginFrame, then Holding
            Holding,   // the captured rows stay published until ReleaseCapture
        };
        std::atomic<CaptureState> m_Capture{ CaptureState::None };
        bool                      m_DisplayCaptured = false; // the published window is a capture
        bool                                         m_Enabled       = true;
        bool                                         m_SortByTime    = true;
        float                                        m_AvgWindowSec  = 0.5f;
        bool                                         m_GpuEnabled    = false; // see GpuEnabled()
        bool                                         m_GpuPassScopes = true;
        bool                                         m_MeshDrawSplit = false;
        bool                                         m_GpuOrderTrace = false;
        std::unordered_map<std::string, DrawSplitResult> m_DrawSplitAccum; // key Pass \x1f Mesh \x1f Material
        std::vector<DrawSplitResult>                     m_DrawSplitDisplay;
        IGpuProfilerSink*                            m_GpuSink       = nullptr;
    };

    // A CPU scope. Timers on one thread form a stack (they nest by C++ scope): each knows the timer it opened
    // inside, and on closing hands that one its inclusive time (for the parent's self time) and the wait time
    // it contained (for the parent's wait column). Closing out of order (a timer outliving one opened after
    // it) is unlinked from the middle of the stack, not mistaken for the innermost one.
    class ScopedTimer
    {
    public:
        // A RUNTIME name (DESERT_PROFILE_SCOPE_DYNAMIC, the backend's pass rows): its row is found in the
        // thread's name cache when the scope OPENS (Profiler::LocalScopeName), and reserved there.
        explicit ScopedTimer( const char* name, ScopeKind kind = ScopeKind::Work );
        // An INTERNED name (a call site's static, DESERT_PROFILE_SCOPE_PREFIXED): its row is known already.
        explicit ScopedTimer( const InternedScopeName& name, ScopeKind kind = ScopeKind::Work );
        // Never throws and never allocates: the constructor did both (Profiler::ReserveSample).
        ~ScopedTimer() noexcept;

        ScopedTimer( const ScopedTimer& )            = delete;
        ScopedTimer& operator=( const ScopedTimer& ) = delete;

    private:
        uint32_t                                       m_NameId    = kNoScopeName; // kNoScopeName: not measured
        ScopedTimer*                                   m_Enclosing = nullptr;
        std::chrono::high_resolution_clock::time_point m_Start;
        double                                         m_NestedMs = 0.0; // inclusive time of the direct children
        double                                         m_WaitMs   = 0.0; // Wait-scope time inside, any depth
        ScopeKind                                      m_Kind;
    };

    // The GPU half of ScopedTimer: brackets the same scope with two device timestamps. Costs one virtual
    // call and, when a command buffer is recording, one vkCmdWriteTimestamp at each end.
    class GpuScopedTimer
    {
    public:
        explicit GpuScopedTimer( const char* name )
        {
            Profiler& profiler = Profiler::Get();
            if ( profiler.GpuEnabled() && profiler.GpuPassScopes() )
            {
                if ( IGpuProfilerSink* sink = profiler.GetGpuSink() )
                {
                    m_Sink   = sink;
                    m_Handle = sink->BeginScope( name );
                }
            }
        }
        ~GpuScopedTimer()
        {
            if ( m_Sink )
                m_Sink->EndScope( m_Handle );
        }

        GpuScopedTimer( const GpuScopedTimer& )            = delete;
        GpuScopedTimer& operator=( const GpuScopedTimer& ) = delete;

    private:
        IGpuProfilerSink* m_Sink   = nullptr;
        int32_t           m_Handle = -1;
    };

    // CONSECUTIVE PHASES OF ONE LONG BODY, each its own row: Next() closes the running phase and opens the
    // named one, and the last phase closes with the timer. For a function whose stages share locals across
    // hundreds of lines (SceneRenderer::OnUpdate builds its graph that way), where a block scope per stage
    // would have to move every local it declares out of the block.
    // A LIFO OF RUNTIME-NAMED CPU SCOPES opened and closed apart (a render graph's pass scopes: BeginPass opens,
    // EndPass closes): in-place slots in pages kept across frames, so a pass costs no heap allocation - the
    // std::unique_ptr<ScopedTimer> per pass this replaces cost one each.
    class ScopedTimerStack
    {
    public:
        ScopedTimerStack() = default;
        ~ScopedTimerStack()
        {
            while ( m_Size > 0 )
                Pop();
        }
        ScopedTimerStack( const ScopedTimerStack& )            = delete;
        ScopedTimerStack& operator=( const ScopedTimerStack& ) = delete;

        void Push( const char* name )
        {
            const std::size_t page = m_Size / kPageSlots;
            if ( page >= m_Pages.size() )
                m_Pages.push_back( std::make_unique<Page>() );
            ( *m_Pages[page] )[m_Size % kPageSlots].emplace( name );
            ++m_Size;
        }
        // Closes the newest scope; nothing when none is open. ~ScopedTimer is noexcept, so this is too.
        void Pop() noexcept
        {
            if ( m_Size == 0 )
                return;
            --m_Size;
            ( *m_Pages[m_Size / kPageSlots] )[m_Size % kPageSlots].reset();
        }
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_Size;
        }
        [[nodiscard]] bool Empty() const noexcept
        {
            return m_Size == 0;
        }

    private:
        static constexpr std::size_t kPageSlots = 32;
        using Page                              = std::array<std::optional<ScopedTimer>, kPageSlots>;
        std::vector<std::unique_ptr<Page>> m_Pages;
        std::size_t                        m_Size = 0;
    };

    class PhaseTimer
    {
    public:
        // @p name: the phase's interned name (DESERT_PROFILE_PHASE interns its literal once, in a static).
        void Next( const InternedScopeName& name )
        {
            m_Current.reset();
            m_Current.emplace( name );
        }
        // Closes the running phase with no successor: for phases nested in one phase of an enclosing
        // PhaseTimer, which must all have closed before that enclosing phase moves on.
        void End()
        {
            m_Current.reset();
        }

    private:
        std::optional<ScopedTimer> m_Current;
    };
} // namespace Common::Profiling

// ── THE SHIPPING BOUNDARY, AND IT IS HERE BECAUSE THE MACROS ARE THE ONLY DOOR ──────────────────────
//
// Every timed scope in this engine goes through one of the macros below — there is no second way in,
// which is what makes a single `#if` enough to remove the whole instrument. Under `Shipping` they expand
// to nothing, so:
//
//   * no `ScopedTimer` is constructed, so `Common::Profiling::Profiler` has no caller and `Profiler.o`
//     is never pulled out of libCommon.a — the aggregator is not in the binary, not merely idle;
//   * no `GpuScopedTimer` is constructed, so the backend's timestamp sink has no caller either;
//   * no `OPTICK_*` expands, so Optick is not linked (the build drops both the library and
//     `USE_OPTICK` under this configuration).
//
// `do {} while(0)` is NOT used: every one of these is a declaration-shaped statement whose whole job is
// to create an object with a lifetime, and four of the six are used at the top of a function body where
// a trailing semicolon after an empty expansion is what the call site already writes. An empty expansion
// leaves exactly that semicolon, which is a null statement and is legal in every position these are used
// in. A `do {} while(0)` would be legal in the same positions but would also silently accept being used
// as an expression, which these never are.
#if !DESERT_DEV_INSTRUMENTS

#define DESERT_PROFILE_SCOPE( NAME )
#define DESERT_PROFILE_WAIT( NAME )
#define DESERT_PROFILE_SCOPE_DYNAMIC( CSTR )
#define DESERT_PROFILE_INTERN_NAME( NAME ) ::Common::Profiling::InternedScopeName{}
#define DESERT_PROFILE_SCOPE_INTERNED( INTERNED )
#define DESERT_PROFILE_SCOPE_PREFIXED( PREFIX, NAME )
#define DESERT_PROFILE_FUNC()
#define DESERT_PROFILE_PASS( NAME )
#define DESERT_PROFILE_PASS_DYNAMIC( CSTR )
#define DESERT_PROFILE_FRAME( NAME )
#define DESERT_PROFILE_PHASES( VAR )
#define DESERT_PROFILE_PHASE( VAR, NAME )
#define DESERT_PROFILE_PHASE_END( VAR )

#else

#define DESERT_PROF_CONCAT_( a, b ) a##b
#define DESERT_PROF_CONCAT( a, b ) DESERT_PROF_CONCAT_( a, b )

// THE CALL SITE'S INTERNED NAME (UE: the TStatId a DECLARE_SCOPE_CYCLE_COUNTER holds): a function-local static,
// so the name table is asked once per call site for the process' life and every later pass reads the id. The
// same reason OPTICK_EVENT keeps a static EventDescription per call site, and the same rule: NAME is a literal.
#define DESERT_PROF_INTERNED_( VAR, NAME )                                                                        \
    static const ::Common::Profiling::InternedScopeName VAR =                                                     \
         ::Common::Profiling::Profiler::Get().InternScopeName( NAME )

// Time a scope: feeds BOTH Optick (external GUI) and the in-engine aggregator (editor panel + logs).
#define DESERT_PROFILE_SCOPE( NAME )                                                                              \
    OPTICK_EVENT( NAME );                                                                                         \
    DESERT_PROF_INTERNED_( DESERT_PROF_CONCAT( _desertProfName_, __LINE__ ), NAME );                              \
    const ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertProf_, __LINE__ )(                          \
         DESERT_PROF_CONCAT( _desertProfName_, __LINE__ ) )

// A scope whose time is the thread BLOCKED (ScopeKind::Wait): a fence, an acquire, a present, a pacer sleep.
// Its own row as DESERT_PROFILE_SCOPE, plus its time in the WaitMs column of every enclosing row; Optick
// draws it in its Wait category. Wrap only the blocking call, so the row is the wait and nothing else.
#define DESERT_PROFILE_WAIT( NAME )                                                                               \
    OPTICK_CATEGORY( NAME, ::Optick::Category::Wait );                                                            \
    DESERT_PROF_INTERNED_( DESERT_PROF_CONCAT( _desertWaitName_, __LINE__ ), NAME );                              \
    const ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertWait_, __LINE__ )(                          \
         DESERT_PROF_CONCAT( _desertWaitName_, __LINE__ ), ::Common::Profiling::ScopeKind::Wait )

// Like DESERT_PROFILE_SCOPE but for a runtime name (e.g. a render-pass name). The const char* must stay
// valid for the duration of the scope; its row is found in the thread's name cache when the scope closes.
// Optick's event of an interned name (Profiler::OptickEventOf): one description per name, not one per call.
#if USE_OPTICK
#define DESERT_PROF_OPTICK_INTERNED_( INTERNED )                                                                  \
    OPTICK_CUSTOM_EVENT( ::Common::Profiling::Profiler::Get().OptickEventOf( INTERNED ) )
#else
#define DESERT_PROF_OPTICK_INTERNED_( INTERNED )
#endif

#define DESERT_PROFILE_SCOPE_DYNAMIC( CSTR )                                                                      \
    const ::Common::Profiling::InternedScopeName DESERT_PROF_CONCAT( _desertProfDynName_, __LINE__ ) =            \
         ::Common::Profiling::Profiler::Get().LocalScopeName( {}, CSTR );                                         \
    DESERT_PROF_OPTICK_INTERNED_( DESERT_PROF_CONCAT( _desertProfDynName_, __LINE__ ) );                          \
    const ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertProfDyn_, __LINE__ )(                       \
         DESERT_PROF_CONCAT( _desertProfDynName_, __LINE__ ) )

// A runtime name interned ONCE by its owner (DESERT_PROFILE_INTERN_NAME, kept beside what it names - the scene's
// systems keep theirs per system) and timed by DESERT_PROFILE_SCOPE_INTERNED with no per-scope lookup: what
// DESERT_PROFILE_SCOPE_DYNAMIC pays every pass (the thread's name cache hashing the characters) is paid once.
// Under Shipping the name is the empty InternedScopeName and the scope expands to nothing.
#define DESERT_PROFILE_INTERN_NAME( NAME ) ::Common::Profiling::Profiler::Get().InternScopeName( NAME )
#define DESERT_PROFILE_SCOPE_INTERNED( INTERNED )                                                                 \
    DESERT_PROF_OPTICK_INTERNED_( INTERNED );                                                                     \
    const ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertProfInt_, __LINE__ )( INTERNED )

// A runtime NAME under a literal PREFIX, as the row "<PREFIX><NAME>" ("RDG Record: " + a pass's name), WITHOUT
// building that string: the thread's name cache finds the row by the two parts' characters
// (Profiler::LocalScopeName), and Optick is handed the table's interned copy, which outlives every scope.
#define DESERT_PROFILE_SCOPE_PREFIXED( PREFIX, NAME )                                                             \
    const ::Common::Profiling::InternedScopeName DESERT_PROF_CONCAT( _desertProfPreName_, __LINE__ ) =            \
         ::Common::Profiling::Profiler::Get().LocalScopeName( PREFIX, NAME );                                     \
    DESERT_PROF_OPTICK_INTERNED_( DESERT_PROF_CONCAT( _desertProfPreName_, __LINE__ ) );                          \
    const ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertProfPre_, __LINE__ )(                       \
         DESERT_PROF_CONCAT( _desertProfPreName_, __LINE__ ) )

// Times the enclosing function, named automatically from the function name — drop one line at the top of
// any method you want to track (don't blanket EVERY tiny getter: the clock read per call adds up and drowns
// the signal — instrument meaningful methods).
#define DESERT_PROFILE_FUNC()                                                                                     \
    OPTICK_EVENT();                                                                                               \
    DESERT_PROF_INTERNED_( DESERT_PROF_CONCAT( _desertProfFnName_, __LINE__ ), __FUNCTION__ );                    \
    const ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertProfFn_, __LINE__ )(                        \
         DESERT_PROF_CONCAT( _desertProfFnName_, __LINE__ ) )

// A RENDER PASS: exactly DESERT_PROFILE_SCOPE plus device timestamps around the same scope, under the
// same name, landing in the same row of the same panel. There is no separate list of pass names anywhere
// — the name written here is the name the profiler shows for both numbers.
//
// Use this at pass granularity ONLY (a dispatch, a render pass, a stage of the frame). The inner scopes
// keep DESERT_PROFILE_SCOPE: a timestamp pair per draw call would cost more than the draw and would say
// nothing a pass total does not, and some of those scopes run hundreds of times a frame.
#define DESERT_PROFILE_PASS( NAME )                                                                               \
    DESERT_PROFILE_SCOPE( NAME );                                                                                 \
    const ::Common::Profiling::GpuScopedTimer DESERT_PROF_CONCAT( _desertGpu_, __LINE__ )( NAME )

// DESERT_PROFILE_PASS for a runtime name (the render graph's pass.Name). The const char* must outlive the
// scope; unlike the CPU side the GPU sink also copies it, because it is read a frame or two later.
#define DESERT_PROFILE_PASS_DYNAMIC( CSTR )                                                                       \
    DESERT_PROFILE_SCOPE_DYNAMIC( CSTR );                                                                         \
    ::Common::Profiling::GpuScopedTimer DESERT_PROF_CONCAT( _desertGpuDyn_, __LINE__ )( CSTR )

// Frame boundary: flip Optick's frame and publish the aggregator's last-frame snapshot. Call once/frame.
#define DESERT_PROFILE_FRAME( NAME )                                                                        \
    OPTICK_FRAME( NAME );                                                                                   \
    ::Common::Profiling::Profiler::Get().BeginFrame()

// Consecutive phases of one body (PhaseTimer): DESERT_PROFILE_PHASES declares the timer, each
// DESERT_PROFILE_PHASE closes the running phase and opens NAME; the last one closes at the end of the scope.
// The phase's name is interned once per call site, in a static of a lambda (an expression, so the macro stays
// one statement wherever it is written).
#define DESERT_PROFILE_PHASES( VAR ) ::Common::Profiling::PhaseTimer VAR
#define DESERT_PROFILE_PHASE( VAR, NAME )                                                                         \
    VAR.Next(                                                                                                     \
         []() -> const ::Common::Profiling::InternedScopeName&                                                    \
         {                                                                                                        \
             DESERT_PROF_INTERNED_( interned, NAME );                                                             \
             return interned;                                                                                     \
         }() )
#define DESERT_PROFILE_PHASE_END( VAR ) VAR.End()

#endif // DESERT_DEV_INSTRUMENTS

// THE MESH DRAW SPLIT'S ONE DOOR (DrawSplitCounts), behind the same boundary as the macros: a mesh pass asks
// MeshDrawSplitOn() once per Record and brackets each draw group with a GpuDrawSplitScope. Under Shipping the
// question is a constant false and the scope an empty object, so no call reaches Profiler or the sink and the
// branches that build the labels fold away.
namespace Common::Profiling
{
#if DESERT_DEV_INSTRUMENTS
    [[nodiscard]] inline bool MeshDrawSplitOn()
    {
        return Profiler::Get().DrawSplitActive();
    }

    // One draw group's timestamp pair: opened by the constructor, closed by the destructor with what Add
    // counted. Built only when MeshDrawSplitOn() said yes; a sink that refuses (-1) makes it a no-op.
    class GpuDrawSplitScope
    {
    public:
        GpuDrawSplitScope( std::string_view pass, std::string_view mesh, std::string_view material )
        {
            if ( IGpuProfilerSink* sink = Profiler::Get().GetGpuSink() )
            {
                m_Sink   = sink;
                m_Handle = sink->BeginDrawScope( pass, mesh, material );
            }
        }
        ~GpuDrawSplitScope()
        {
            if ( m_Sink )
                m_Sink->EndDrawScope( m_Handle, m_Counts );
        }

        GpuDrawSplitScope( const GpuDrawSplitScope& )            = delete;
        GpuDrawSplitScope& operator=( const GpuDrawSplitScope& ) = delete;

        // @p triangles of all instances drawn, or of one when @p perInstance (the GPU owns the instance count).
        void Add( uint32_t draws, uint64_t triangles, bool perInstance )
        {
            m_Counts.Draws += draws;
            m_Counts.Triangles += triangles;
            m_Counts.TrianglesPerInstance = m_Counts.TrianglesPerInstance || perInstance;
        }

    private:
        IGpuProfilerSink* m_Sink   = nullptr;
        int32_t           m_Handle = -1;
        DrawSplitCounts   m_Counts;
    };
#else
    [[nodiscard]] constexpr bool MeshDrawSplitOn()
    {
        return false;
    }

    class GpuDrawSplitScope
    {
    public:
        GpuDrawSplitScope( std::string_view, std::string_view, std::string_view )
        {
        }
        GpuDrawSplitScope( const GpuDrawSplitScope& )            = delete;
        GpuDrawSplitScope& operator=( const GpuDrawSplitScope& ) = delete;
        void               Add( uint32_t, uint64_t, bool )
        {
        }
    };
#endif // DESERT_DEV_INSTRUMENTS
} // namespace Common::Profiling
