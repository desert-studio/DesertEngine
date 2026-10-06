#include "VFXClock.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::VFX
{
    namespace
    {
        // A step is "whole" a ten-thousandth of a step early. Frame deltas arrive as float seconds
        // (Common::Timestep), and 1/60 in float is a hair OFF 1/60 in double — without the tolerance a
        // frame of exactly one step would sometimes be 0.99999999 of one, run nothing, and run two the
        // frame after: a stutter at the very rate the step was chosen to match.
        constexpr double kWholeStepTolerance = 1e-4;

        std::uint64_t WholeSteps( double seconds, double stepSeconds )
        {
            if ( seconds <= 0.0 )
                return 0;
            return static_cast<std::uint64_t>( std::floor( seconds / stepSeconds + kWholeStepTolerance ) );
        }
    } // namespace

    Clock::Clock( const ClockSettings& settings ) : m_Settings( settings )
    {
    }

    void Clock::Reset()
    {
        m_Step         = 0;
        m_Accumulator  = 0.0;
        m_ResetPending = true;
        m_SeekPending  = false;
    }

    void Clock::SeekTo( double age )
    {
        m_SeekPending = true;
        m_SeekAge     = std::max( age, 0.0 );
    }

    TickPlan Clock::Seek()
    {
        TickPlan            plan;
        const std::uint64_t target = WholeSteps( m_SeekAge, m_Settings.StepSeconds );

        // Backwards: there is no inverse step, so the only way to an earlier age is from zero
        // (NiagaraComponent.cpp:1003-1007, ResetAll then recompute the difference).
        if ( target < m_Step || m_ResetPending )
        {
            plan.Reset     = true;
            m_Step         = 0;
            m_ResetPending = false;
        }

        const std::uint64_t remaining = target - m_Step;
        const std::uint64_t run       = std::min<std::uint64_t>( remaining, m_Settings.MaxSeekStepsPerTick );

        plan.FirstStep = m_Step;
        plan.StepCount = static_cast<std::uint32_t>( run );
        m_Step += run;

        // The seek is served when the age is reached; ordinary time resumes from there, with no partial
        // step owed — the seek age is quantised to whole steps, as every other age is.
        m_SeekPending = m_Step < target;
        plan.Seeking  = m_SeekPending;
        m_Accumulator = 0.0;
        return plan;
    }

    TickPlan Clock::Advance( double seconds )
    {
        if ( m_SeekPending )
            return Seek();

        TickPlan plan;
        if ( m_ResetPending )
        {
            plan.Reset     = true;
            m_ResetPending = false;
        }

        m_Accumulator += std::max( seconds, 0.0 );
        const std::uint64_t whole = WholeSteps( m_Accumulator, m_Settings.StepSeconds );
        const std::uint64_t run   = std::min<std::uint64_t>( whole, m_Settings.MaxStepsPerTick );

        m_Accumulator -= static_cast<double>( whole ) * m_Settings.StepSeconds;
        // A hitch past the budget is dropped, not owed (see ClockSettings::MaxStepsPerTick): the steps
        // beyond `run` are subtracted above along with the ones that ran. The tolerance can leave a
        // sliver below zero; it is time already counted, so it is not carried as debt either.
        m_Accumulator = std::max( m_Accumulator, 0.0 );

        plan.FirstStep = m_Step;
        plan.StepCount = static_cast<std::uint32_t>( run );
        m_Step += run;
        return plan;
    }

    std::uint32_t SpawnAccumulator::Step( double ratePerSecond, double stepSeconds )
    {
        Carry += std::max( ratePerSecond, 0.0 ) * stepSeconds;
        const double whole = std::floor( Carry );
        Carry -= whole;
        return static_cast<std::uint32_t>( whole );
    }
} // namespace Desert::VFX
