#include <Common/Core/Math/Rounding.hpp>

#include <cstdint>
#include "Profiler.hpp"

#include <algorithm>

namespace Common::Profiling
{
    Profiler& Profiler::Get()
    {
        static Profiler s_Instance;
        return s_Instance;
    }

    void Profiler::BeginFrame()
    {
        std::lock_guard<std::mutex> lock( m_Mutex );

        m_FrameThread  = std::this_thread::get_id();
        const auto now = Clock::now();
        if ( !m_WindowStarted )
        {
            m_WindowStart   = now;
            m_WindowStarted = true;
        }
        ++m_WindowFrames;

        const double windowMs    = std::chrono::duration<double, std::milli>( now - m_WindowStart ).count();
        const double windowLimit = static_cast<double>( m_AvgWindowSec ) * 1000.0;
        if ( windowMs < windowLimit || m_WindowFrames == 0 )
            return; // still accumulating this window

        // Window elapsed: publish the AVERAGE per-frame cost of every scope over the window.
        const double inv = 1.0 / static_cast<double>( m_WindowFrames );
        m_Display.clear();
        m_Display.reserve( m_Accum.size() );
        for ( const auto& [name, result] : m_Accum )
        {
            ScopeResult avg;
            avg.Name      = name;
            avg.TotalMs   = result.TotalMs * inv;                              // avg ms / frame
            avg.Calls     = static_cast<uint32_t>( Math::RoundToNearest( result.Calls * inv ) ); // avg calls/frame
            avg.SelfMs    = result.SelfMs * inv;
            avg.WaitMs    = result.WaitMs * inv;
            avg.GpuMs     = result.GpuMs * inv;                                // avg GPU ms / frame
            avg.GpuSelfMs = result.GpuSelfMs * inv;
            avg.GpuCalls  = static_cast<uint32_t>( Math::RoundToNearest( result.GpuCalls * inv ) );
            m_Display.push_back( avg );
        }
        if ( m_SortByTime )
            std::sort( m_Display.begin(), m_Display.end(),
                       []( const ScopeResult& a, const ScopeResult& b ) { return a.TotalMs > b.TotalMs; } );
        else
            std::sort( m_Display.begin(), m_Display.end(),
                       []( const ScopeResult& a, const ScopeResult& b ) { return a.Name < b.Name; } );

        m_DisplayFrameMs      = windowMs * inv; // avg frame time over the window
        m_DisplayFrameNamedMs = m_AccumFrameNamedMs * inv;
        m_AccumFrameNamedMs   = 0.0;

        // The mesh draw split's groups, averaged over the same frames (empty when the split was off all window).
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
        std::sort( m_DrawSplitDisplay.begin(), m_DrawSplitDisplay.end(),
                   []( const DrawSplitResult& a, const DrawSplitResult& b )
                   { return a.Pass != b.Pass ? a.Pass < b.Pass : a.GpuMs > b.GpuMs; } );
        m_DrawSplitAccum.clear();

        m_Accum.clear();
        m_WindowFrames = 0;
        m_WindowStart  = now;
    }

    void Profiler::AddSample( const char* name, double totalMs, double selfMs, double waitMs, bool outermost )
    {
        if ( !m_Enabled )
            return;

        std::lock_guard<std::mutex> lock( m_Mutex );
        if ( outermost && std::this_thread::get_id() == m_FrameThread )
            m_AccumFrameNamedMs += totalMs;
        auto&                       r = m_Accum[name]; // summed across the whole window, averaged on publish
        if ( r.Name.empty() )
            r.Name = name;
        r.TotalMs += totalMs;
        r.SelfMs += selfMs;
        r.WaitMs += waitMs;
        r.Calls += 1;
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

            Profiler::Get().AddSample( m_Name, totalMs, std::max( 0.0, totalMs - m_NestedMs ), waitMs,
                                       m_Enclosing == nullptr );
        }
        DESERT_DESTRUCTOR_GUARD( "~ScopedTimer" )
    }

    void Profiler::AddGpuSample( const char* name, double inclusiveMs, double selfMs )
    {
        if ( !m_Enabled )
            return;

        // The backend resolves a frame's queries MaxFramesInFlight frames after they were written, so this
        // lands in a LATER window than the CPU sample of the same scope. Over a window of hundreds of
        // frames that shifts the average by the lag divided by the window — well under a per cent — and it
        // keeps the read off the GPU's critical path, which is the whole point.
        std::lock_guard<std::mutex> lock( m_Mutex );
        auto&                       r = m_Accum[name];
        if ( r.Name.empty() )
            r.Name = name;
        r.GpuMs += inclusiveMs;
        r.GpuSelfMs += selfMs;
        r.GpuCalls += 1;
    }

    void Profiler::AddDrawSplitSample( std::string_view pass, std::string_view mesh, std::string_view material,
                                       double gpuMs, const DrawSplitCounts& counts )
    {
        if ( !m_Enabled )
            return;

        std::string key;
        key.reserve( pass.size() + mesh.size() + material.size() + 2 );
        key.append( pass ).append( 1, '' ).append( mesh ).append( 1, '' ).append( material );

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
