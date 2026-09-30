#include "EngineStats.hpp"

#include <Engine/Core/Glfw.hpp>

#include <spdlog/fmt/fmt.h>

namespace Desert::Engine
{
    void EngineStats::Update()
    {
        float currentTime = static_cast<float>( glfwGetTime() );
        m_FrameTime       = Common::Timestep( currentTime - m_LastFrameTime );
        m_LastFrameTime   = Common::Timestep( currentTime );

        // FPS calculation logic remains the same
        m_FPSCounter++;
        m_FPSAccumulator += m_FrameTime.GetSeconds();
        if ( m_FPSAccumulator >= m_FPSUpdateInterval )
        {
            m_FPS            = static_cast<float>( m_FPSCounter ) / m_FPSAccumulator;
            m_FPSCounter     = 0;
            m_FPSAccumulator = 0.0f;
        }
    }

    std::string EngineStats::GetFormattedStats() const
    {
        // TWO DECIMALS, AND WHY THE DEFAULT WAS NOT MERELY UGLY. This was
        // `std::to_string( GetFrameTimeMs() )`, and std::to_string(float) writes SIX decimals — so the
        // editor's title bar read "Frame: 6.252289ms" and the last four digits changed every frame. They
        // are not measurement: the frame time comes from a float difference of two glfwGetTime() samples,
        // whose own resolution does not reach a microsecond, and nothing else in the engine prints a
        // duration to more than three. Six digits of noise beside a number a person is watching for a
        // trend is precision claimed and not held; two is what the profiler, the status bar and the perf
        // HUD all print.
        //
        // The FPS beside it is a ONE-SECOND average and this millisecond figure is the LAST FRAME, so the
        // two halves of this line answer over different windows and are only expected to agree at a
        // steady state. That is deliberate — the average is what a person reads, the instant is what a
        // hitch shows — and it is said here because the line does not say it.
        return fmt::format( "FPS: {} | Frame: {:.2f}ms", static_cast<int>( m_FPS ), GetFrameTimeMs() );
    }
} // namespace Desert::Engine