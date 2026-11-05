#pragma once

#include <chrono>
#include <cstdint>

namespace Desert::Engine
{
    // THE FRAME RATE LIMIT — UE's t.MaxFPS, which UGameUserSettings::FrameRateLimit sets. The main loop
    // (Application::Run) asks it to wait once per frame after present; with a limit of N it holds each frame
    // to 1/N s, on a fixed schedule rather than "sleep the remainder" so the error of one sleep does not add
    // up. A frame that ran late starts the schedule over instead of being followed by a burst of short ones.
    class FramePacer
    {
    public:
        FramePacer() = default;
        ~FramePacer();
        FramePacer( const FramePacer& )            = delete;
        FramePacer& operator=( const FramePacer& ) = delete;

        // 0 = not limited.
        void                   SetLimit( uint32_t framesPerSecond );
        [[nodiscard]] uint32_t Limit() const
        {
            return m_Limit;
        }

        // Blocks until the next frame may start; returns at once when not limited.
        void WaitForNextFrame();

    private:
        using Clock = std::chrono::steady_clock;

        // UE FPlatformProcess::Sleep's shape for a frame cap: the OS sleeps to 0.5 ms before @p until on a
        // high-resolution waitable timer (Windows; a plain sleep_until is 15.6 ms coarse there), then the thread
        // spins the rest, so a cap of 120 holds 120 and not the 64 the coarse tick rounds it to.
        void SleepUntil( Clock::time_point until );

        void* m_Timer        = nullptr; // Windows: the high-resolution waitable timer, made on first use
        bool  m_TimerRefused = false;   // Windows: the timer was refused once (said in the log); sleep_until then

        uint32_t          m_Limit = 0;
        Clock::time_point m_Next{};
    };
} // namespace Desert::Engine
