#pragma once

/**
 * THE PLAYER: WHERE PLAYBACK IS. TIME ONLY — IT NEVER TOUCHES A SEQUENCE'S KEYS OR A LIVE OBJECT.
 *
 * UE's UMovieSceneSequencePlayer minus everything that is not time. The Animator's `ClipPlayback`
 * (FrameTime + Loop), the Sequencer panel's playhead and `UIAnimData::Playing`/`Loop` are three clocks
 * today; each becomes one of these. What it hands the evaluator is a STEP, not a time: events fire on
 * the interval crossed, so a player that only reported "now" would lose every event between two frames.
 *
 * INVARIANTS: `Current()` is always inside [Start, End] (subframe in [0, 1)); `Advance` never skips more
 * than one loop per call (a frame longer than the range is clamped to one wrap and reported `Wrapped`);
 * the rate is signed — negative plays backwards, 0 is paused-but-Playing (UE allows it and a time dilation
 * of 0 is a real game state).
 */

#include <Engine/Animation/TimeModel.hpp>

#include <cstdint>

namespace Desert::Animation::Timeline
{
    /// STORED (UI clips and LevelSequence actors persist it): append only.
    enum class LoopMode : uint8_t
    {
        Once     = 0, ///< stop on the last tick, holding it
        Loop     = 1,
        PingPong = 2, ///< reverse direction at each end
    };

    enum class PlayState : uint8_t
    {
        Stopped,
        Playing,
        Paused,
    };

    /// What one `Advance` covered — the evaluator's input.
    /// Which way the playhead moves (UE FMovieSceneEvaluationRange::Direction).
    enum class PlayDirection : uint8_t
    {
        Forward,
        Backward,
    };

    struct TimeStep
    {
        FrameTime From;
        FrameTime To;
        /// The way the step STARTS. A wrap or a PingPong turn is taken this way round — stated by the Player
        /// that moved, not guessed from the ends by the Evaluator.
        PlayDirection Direction = PlayDirection::Forward;
        bool      Wrapped  = false; ///< crossed End→Start (or Start→End backwards) once
        bool      Reversed = false; ///< PingPong turned around inside this step
        bool      Finished = false; ///< Once mode reached the end in this step; the state is now Stopped
    };

    class Player
    {
    public:
        Player( FrameRate tickRate, FrameNumber start, FrameNumber end );

        void Play();
        void Pause();
        /// Back to Start, Stopped. A stop is a jump: it crosses no events.
        void Stop();

        void SetLoopMode( LoopMode mode );
        void SetPlayRate( double rate );
        /// Jump without crossing events (scrubbing) — the returned step has From == To.
        [[nodiscard]] TimeStep JumpTo( FrameTime at );

        /// Advance by @p seconds of game time × play rate. Not Playing → an empty step at Current().
        [[nodiscard]] TimeStep Advance( double seconds );

        [[nodiscard]] FrameTime Current() const
        {
            return m_Current;
        }
        [[nodiscard]] PlayState State() const
        {
            return m_State;
        }

    private:
        FrameRate   m_TickRate;
        FrameNumber m_Start;
        FrameNumber m_End;
        FrameTime   m_Current;
        double      m_Rate      = 1.0;
        int32_t     m_Direction = 1;
        LoopMode    m_Loop      = LoopMode::Once;
        PlayState   m_State     = PlayState::Stopped;
    };
} // namespace Desert::Animation::Timeline
