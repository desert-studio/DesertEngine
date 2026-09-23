#pragma once

#include <Common/Core/Timestep.hpp>
#include <algorithm>
#include <cstdint>
#include <string>

namespace Desert::Engine
{
    class EngineStats
    {
    public:
        EngineStats()  = default;
        ~EngineStats() = default;

        void Update();

        float GetFPS() const
        {
            return m_FPS;
        }

        float GetFrameTimeMs() const
        {
            return m_FrameTime.GetMilliseconds();
        }

        auto GetDeltaTime() const
        {
            return Common::Timestep( std::min<float>( m_FrameTime.GetSeconds(), 0.0333F ) );
        }

        std::string GetFormattedStats() const;

    private:
        float            m_FPS = 0.0f;
        Common::Timestep m_FrameTime;
        Common::Timestep m_LastFrameTime;

        float    m_FPSUpdateInterval = 1.0f;
        float    m_FPSAccumulator    = 0.0f;
        uint32_t m_FPSCounter        = 0;
    };
} // namespace Desert::Engine