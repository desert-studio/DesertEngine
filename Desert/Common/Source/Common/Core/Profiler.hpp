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

#include <Common/Core/DestructorGuard.hpp>
#include <optick.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Common::Profiling
{
    struct ScopeResult
    {
        std::string Name;
        double      TotalMs = 0.0; // summed across all calls this frame
        uint32_t    Calls   = 0;
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
    };

    // The row the backend publishes the whole-frame GPU bracket under. It is a DENOMINATOR, not a pass:
    // anything summing the per-pass column has to leave it out. Declared here, beside the results it
    // appears in, so the backend that writes it and the panel that reads it share one string.
    inline constexpr const char* kGpuFrameTotalScope = "GPU: Frame Total";

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
    };

    // Main-thread oriented CPU scope aggregator. Thread-safe (cheap mutex) so worker-thread scopes don't
    // corrupt the map, but it's designed around the render/update thread.
    class Profiler
    {
    public:
        static Profiler& Get();

        // Per-frame boundary. Accumulates scope times across all frames in the current averaging window;
        // when the window (AvgWindowSeconds) elapses it publishes the AVERAGE per-frame time of each scope.
        // Averaging makes the numbers readable at high FPS (per-frame values flicker too fast to read).
        void BeginFrame();

        void AddSample( const char* name, double ms );

        // Resolved GPU time for a scope, fed by the backend once the queries for a finished frame are
        // readable. Lands in the same row as the CPU sample of the same name. @p inclusiveMs covers nested
        // passes, @p selfMs has them subtracted.
        void AddGpuSample( const char* name, double inclusiveMs, double selfMs );

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

        // Published, averaged-over-the-window results (see BeginFrame).
        const std::vector<ScopeResult>& LastFrame() const
        {
            return m_Display;
        }
        double LastFrameMs() const
        {
            return m_DisplayFrameMs;
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

    private:
        using Clock = std::chrono::high_resolution_clock;

        std::mutex                                   m_Mutex;
        std::unordered_map<std::string, ScopeResult> m_Accum; // summed over the current window (not per-frame)
        std::vector<ScopeResult>                     m_Display;
        double                                       m_DisplayFrameMs = 0.0;
        Clock::time_point                            m_WindowStart;
        uint32_t                                     m_WindowFrames  = 0;
        bool                                         m_WindowStarted = false;
        bool                                         m_Enabled       = true;
        bool                                         m_SortByTime    = true;
        float                                        m_AvgWindowSec  = 0.5f;
        bool                                         m_GpuEnabled    = false; // see GpuEnabled()
        bool                                         m_GpuPassScopes = true;
        IGpuProfilerSink*                            m_GpuSink       = nullptr;
    };

    class ScopedTimer
    {
    public:
        explicit ScopedTimer( const char* name )
            : m_Name( name ), m_Start( std::chrono::high_resolution_clock::now() )
        {
        }
        ~ScopedTimer()
        try
        {
            const auto end = std::chrono::high_resolution_clock::now();
            Profiler::Get().AddSample(
                 m_Name, std::chrono::duration<double, std::milli>( end - m_Start ).count() );
        }
        DESERT_DESTRUCTOR_GUARD( "~ScopedTimer" )

    private:
        const char*                                    m_Name;
        std::chrono::high_resolution_clock::time_point m_Start;
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
} // namespace Common::Profiling

#define DESERT_PROF_CONCAT_( a, b ) a##b
#define DESERT_PROF_CONCAT( a, b ) DESERT_PROF_CONCAT_( a, b )

// Time a scope: feeds BOTH Optick (external GUI) and the in-engine aggregator (editor panel + logs).
#define DESERT_PROFILE_SCOPE( NAME )                                                                        \
    OPTICK_EVENT( NAME );                                                                                   \
    ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertProf_, __LINE__ )( NAME )

// Like DESERT_PROFILE_SCOPE but for a runtime name (e.g. a render-pass name). The const char* must stay
// valid for the duration of the scope.
#define DESERT_PROFILE_SCOPE_DYNAMIC( CSTR )                                                                \
    OPTICK_EVENT_DYNAMIC( CSTR );                                                                           \
    ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertProfDyn_, __LINE__ )( CSTR )

// Times the enclosing function, named automatically from the function name — drop one line at the top of
// any method you want to track (don't blanket EVERY tiny getter: the clock read + map insert per call adds
// up and drowns the signal — instrument meaningful methods).
#define DESERT_PROFILE_FUNC()                                                                               \
    OPTICK_EVENT();                                                                                         \
    ::Common::Profiling::ScopedTimer DESERT_PROF_CONCAT( _desertProfFn_, __LINE__ )( __FUNCTION__ )

// A RENDER PASS: exactly DESERT_PROFILE_SCOPE plus device timestamps around the same scope, under the
// same name, landing in the same row of the same panel. There is no separate list of pass names anywhere
// — the name written here is the name the profiler shows for both numbers.
//
// Use this at pass granularity ONLY (a dispatch, a render pass, a stage of the frame). The inner scopes
// keep DESERT_PROFILE_SCOPE: a timestamp pair per draw call would cost more than the draw and would say
// nothing a pass total does not, and some of those scopes run hundreds of times a frame.
#define DESERT_PROFILE_PASS( NAME )                                                                               \
    DESERT_PROFILE_SCOPE( NAME );                                                                                 \
    ::Common::Profiling::GpuScopedTimer DESERT_PROF_CONCAT( _desertGpu_, __LINE__ )( NAME )

// DESERT_PROFILE_PASS for a runtime name (the render graph's pass.Name). The const char* must outlive the
// scope; unlike the CPU side the GPU sink also copies it, because it is read a frame or two later.
#define DESERT_PROFILE_PASS_DYNAMIC( CSTR )                                                                       \
    DESERT_PROFILE_SCOPE_DYNAMIC( CSTR );                                                                         \
    ::Common::Profiling::GpuScopedTimer DESERT_PROF_CONCAT( _desertGpuDyn_, __LINE__ )( CSTR )

// Frame boundary: flip Optick's frame and publish the aggregator's last-frame snapshot. Call once/frame.
#define DESERT_PROFILE_FRAME( NAME )                                                                        \
    OPTICK_FRAME( NAME );                                                                                   \
    ::Common::Profiling::Profiler::Get().BeginFrame()
