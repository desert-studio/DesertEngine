#pragma once

#include <cstdint>

namespace Desert::VFX
{
    // How the effects clock turns a frame's time into simulation steps.
    struct ClockSettings
    {
        // Every step is this long. FIXED, never the frame's delta: a step of variable length makes the
        // simulation a function of the frame rate, and the same scene then looks different at 30 and at
        // 144 Hz, in a scrub and in playback, on the first run and the second.
        double StepSeconds = 1.0 / 60.0;

        // Most steps one ordinary tick may run. A hitch longer than this many steps is DROPPED, not
        // caught up (UE: MaxSimTime, NiagaraComponent.cpp:650) — catching up a two-second stall would
        // spend the next frame on 120 steps and stall again. The effects slow down for that frame instead.
        std::uint32_t MaxStepsPerTick = 4;

        // Most steps one SEEK tick may run (UE: MultiTick's MaxSimTime budget, NiagaraComponent.cpp:1020).
        // A seek further than this is spread over several ticks; IsSeeking says one is still in progress.
        std::uint32_t MaxSeekStepsPerTick = 60;
    };

    // What one tick of the clock asks the simulation to do.
    struct TickPlan
    {
        // The state must be thrown away BEFORE the steps below: a seek went backwards, or Reset was called.
        bool Reset = false;

        // Index of the first step of this tick, counted from the last reset. A step's index is its
        // identity — spawn ids and random numbers are derived from it, never from wall time.
        std::uint64_t FirstStep = 0;

        // How many fixed steps of ClockSettings::StepSeconds to run, in order.
        std::uint32_t StepCount = 0;

        // A seek ran out of budget this tick and continues in the next one.
        bool Seeking = false;
    };

    // THE EFFECTS CLOCK OF ONE WORLD: fixed steps from the scene's time, and seeking by replay.
    //
    // Two modes, as UE's `ENiagaraAgeUpdateMode`: Advance is TickDeltaTime (time flows by the scene's
    // delta), SeekTo is DesiredAge (the age is SET — by a scrub, a Sequencer track, a restart). A seek is
    // the port of the DesiredAge branch of UNiagaraComponent::TickComponent (NiagaraComponent.cpp:995-
    // 1032): backwards is a reset followed by stepping forward from zero; forwards is floor(diff / step)
    // fixed steps under a per-tick budget. There is no other way to reach an age, which is what makes an
    // age reached by scrubbing the same state as the age reached by playing.
    //
    // Pure arithmetic — no GPU, no ECS — so the suite VFXClock checks it alone.
    class Clock
    {
    public:
        explicit Clock( const ClockSettings& settings = {} );

        // Ordinary time: accumulate `seconds` (the scene's delta, zero while paused) and run the whole
        // steps that fit, under MaxStepsPerTick. While a seek is pending the delta is ignored and the
        // seek is served instead.
        [[nodiscard]] TickPlan Advance( double seconds );

        // Ask for the state at `age` seconds after the last reset. Served by the next Advance calls.
        void SeekTo( double age );

        // Back to age zero; the next plan carries Reset.
        void Reset();

        [[nodiscard]] std::uint64_t GetStep() const
        {
            return m_Step;
        }
        [[nodiscard]] double GetAge() const
        {
            return static_cast<double>( m_Step ) * m_Settings.StepSeconds;
        }
        [[nodiscard]] bool IsSeeking() const
        {
            return m_SeekPending;
        }
        [[nodiscard]] const ClockSettings& GetSettings() const
        {
            return m_Settings;
        }

    private:
        [[nodiscard]] TickPlan Seek();

        ClockSettings m_Settings;
        std::uint64_t m_Step         = 0;   // steps run since the last reset
        double        m_Accumulator  = 0.0; // time not yet turned into a step, in seconds
        bool          m_ResetPending = false;
        bool          m_SeekPending  = false;
        double        m_SeekAge      = 0.0;
    };

    // The spawn count of each step for one emitter: a rate turned into whole particles with the fraction
    // carried to the next step, so a rate of 0.5/step spawns every other step instead of never. Kept in
    // double and advanced only per fixed step, so it is the same sequence on every run and platform.
    struct SpawnAccumulator
    {
        double Carry = 0.0;

        [[nodiscard]] std::uint32_t Step( double ratePerSecond, double stepSeconds );
    };
} // namespace Desert::VFX
