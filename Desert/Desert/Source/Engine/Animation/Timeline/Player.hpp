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

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Desert::Animation::Timeline
{
    /// STORED (UI clips and LevelSequence actors persist it): append only.
    enum class LoopMode : uint8_t
    {
        Once     = 0, ///< stop on the last tick, holding it
        Loop     = 1,
        PingPong = 2, ///< reverse direction at each end
    };

    /// Every LoopMode in stored order - what a picker lists (the Details combo) and what bounds the reflected
    /// range.
    inline constexpr std::array<LoopMode, 3> kLoopModes{ LoopMode::Once, LoopMode::Loop, LoopMode::PingPong };

    /// THE ONE HOME OF LoopMode'S NAMES: the reflected enumerator name, the same text the scene stores
    /// (ComponentRegistry.cpp writes the enum through reflection). A value outside the enum names itself
    /// "Unknown".
    [[nodiscard]] const char* ToString( LoopMode mode );
    /// The reflected name back to the mode; any other text is nullopt, never a default.
    [[nodiscard]] std::optional<LoopMode> LoopModeFromString( std::string_view name );

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

namespace rfl::config
{
    template <typename T> struct enum_range;

    /// LoopMode is persisted BY NAME (the scene is a text format a person reads). Its underlying type is
    /// uint8_t, and reflect-cpp's default scan range is `int` — which does not compile against a uint8_t
    /// enum — so its range is given in its own type.
    template <> struct enum_range<Desert::Animation::Timeline::LoopMode>
    {
        static constexpr uint8_t min = 0;
        static constexpr uint8_t max = static_cast<uint8_t>( Desert::Animation::Timeline::kLoopModes.size() - 1 );
    };
} // namespace rfl::config
