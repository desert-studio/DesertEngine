// Ported from UE 5.8 Engine/Source/Runtime/Engine/Private/LevelTick.cpp:1565-1610 (UWorld::Tick time
// accounting) and Private/WorldSettings.cpp:334-348 (FixupDeltaSeconds, SetTimeDilation), adapted: one
// value type owned by Core::Scene instead of fields on UWorld/AWorldSettings; no audio/unpaused clocks;
// the editor's viewport Realtime toggle and the headless capture's fixed step are inputs of Tick().
#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>

namespace Desert::Core
{
    /// What the world's clock does this frame. Decided by the scene from its state and the editor's
    /// Realtime toggle (ClockModeFor), so the rule lives in one pure function the suite can assert.
    enum class WorldClockMode : uint8_t
    {
        Frozen,  // editing with Realtime off, or the scene is paused: nothing moves
        Preview, // editing with Realtime on: the world's look moves (wind, particles, material Time)
        Game     // Play: gameplay time
    };

    /**
     * THE WORLD'S CLOCK — one per Scene, and the only time anything that moves in the picture may read.
     *
     * Time lives with the world, not with the sky or a renderer (UE: UWorld::TimeSeconds /
     * RealTimeSeconds / DeltaTimeSeconds, pause, AWorldSettings::TimeDilation). Before this every consumer
     * kept its own clock — a steady_clock origin in the particle renderer and in the material Time
     * uniform, a gameplay-timestep accumulator for the cloud wind, a constant 0.016 s for eye adaptation —
     * so pausing stopped some of them and not others, and a headless capture of a scene with particles was
     * a picture of how long the process had been running.
     *
     *   GameTime  — advances by Delta: frozen when paused / Frozen, scaled by TimeDilation.
     *   RealTime  — wall clock since the world began; advances every Tick, paused or not.
     *   Delta     — this frame's game step (0 when frozen).
     *
     * A fixed step (headless capture) replaces the measured step for GAME time only: two captures of one
     * scene then see the same world at every frame, while RealTime still says how long it actually took.
     */
    class WorldTime
    {
    public:
        // UE 5.8 AWorldSettings defaults (MaxUndilatedFrameTime, Min/MaxGlobalTimeDilation). The frame
        // clamp is what keeps a hitch (a breakpoint, a shader compile) from firing a second of simulation
        // in one step.
        static constexpr float kMaxUndilatedFrameSeconds = 0.4f;
        static constexpr float kMinTimeDilation          = 0.0001f;
        static constexpr float kMaxTimeDilation          = 20.0f;

        static constexpr WorldClockMode ClockModeFor( bool playing, bool pausedState, bool previewRealtime )
        {
            if ( pausedState )
                return WorldClockMode::Frozen;
            if ( playing )
                return WorldClockMode::Game;
            return previewRealtime ? WorldClockMode::Preview : WorldClockMode::Frozen;
        }

        void Tick( float realDeltaSeconds, WorldClockMode mode )
        {
            m_RealDelta = std::max( realDeltaSeconds, 0.0f );
            m_RealTime += m_RealDelta;
            m_Mode = mode;

            const bool advancing = mode != WorldClockMode::Frozen && !m_Paused;
            if ( !advancing )
            {
                m_Delta = 0.0f;
                return;
            }
            const float step = m_FixedStep.value_or( std::min( m_RealDelta, kMaxUndilatedFrameSeconds ) );
            m_Delta          = step * m_TimeDilation;
            m_GameTime += m_Delta;
        }

        /// Back to zero, as a world that has just begun (entering Play: UE's PIE world starts at 0).
        void Reset()
        {
            m_GameTime  = 0.0;
            m_RealTime  = 0.0;
            m_Delta     = 0.0f;
            m_RealDelta = 0.0f;
            m_Paused    = false;
        }

        void SetPaused( bool paused )
        {
            m_Paused = paused;
        }
        [[nodiscard]] bool IsPaused() const
        {
            return m_Paused;
        }

        /// Refused outside UE's range rather than clamped: a zero or negative dilation from a script is a
        /// defect in the script, and silently turning it into 0.0001 would hide it.
        [[nodiscard]] Common::BoolResultStr SetTimeDilation( float dilation )
        {
            if ( !std::isfinite( dilation ) || dilation < kMinTimeDilation || dilation > kMaxTimeDilation )
                return Common::MakeError( std::format( "WorldTime: time dilation {} is outside [{}, {}]", dilation,
                                                       kMinTimeDilation, kMaxTimeDilation ) );
            m_TimeDilation = dilation;
            return BOOLSUCCESS;
        }
        [[nodiscard]] float GetTimeDilation() const
        {
            return m_TimeDilation;
        }

        /// A headless capture's step. nullopt = the measured frame time drives the game clock.
        void SetFixedStep( std::optional<float> seconds )
        {
            m_FixedStep = seconds;
        }
        [[nodiscard]] std::optional<float> GetFixedStep() const
        {
            return m_FixedStep;
        }

        [[nodiscard]] double GetGameTimeSeconds() const
        {
            return m_GameTime;
        }
        [[nodiscard]] double GetRealTimeSeconds() const
        {
            return m_RealTime;
        }
        [[nodiscard]] float GetDeltaSeconds() const
        {
            return m_Delta;
        }
        [[nodiscard]] float GetRealDeltaSeconds() const
        {
            return m_RealDelta;
        }
        [[nodiscard]] WorldClockMode GetMode() const
        {
            return m_Mode;
        }

    private:
        double               m_GameTime     = 0.0;
        double               m_RealTime     = 0.0;
        float                m_Delta        = 0.0f;
        float                m_RealDelta    = 0.0f;
        float                m_TimeDilation = 1.0f;
        bool                 m_Paused       = false;
        WorldClockMode       m_Mode         = WorldClockMode::Frozen;
        std::optional<float> m_FixedStep;
    };
} // namespace Desert::Core
