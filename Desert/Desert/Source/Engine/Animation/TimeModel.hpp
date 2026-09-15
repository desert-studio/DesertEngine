#pragma once

/**
 * TIME IS AN INTEGER COUNT OF TICKS, AND THE RATE IT IS COUNTED AT IS A RATIONAL NUMBER.
 *
 * Report 05 §931 puts this first in the dependency order and adds the sentence that dates it: "Do this
 * before you serialise anything. The migration machinery in `MovieSceneFrameMigration.h` and the
 * `LegacyConversionFrameRate` CVar exist because Epic did it in the wrong order." This engine has six
 * `.anim` files, so it is the cheapest this decision will ever be.
 *
 * WHAT WAS WRONG WITH FLOAT SECONDS, measured on this tree rather than argued from the article:
 *
 *   - THE UNIT IN THE FORMAT WAS FICTION. `AnimationClip::TicksPerSecond` existed, and every one of the
 *     six shipped clips set it to 1.0 — so a "tick" was a second and 210 of the corpus's 267 key times
 *     were fractional. Two suites had codified that ("Key times ARE seconds when TicksPerSecond is 1"),
 *     which is how a unit nobody used survives.
 *   - EQUALITY OF TWO TIMES WAS NOT DECIDABLE. `0.1f + 0.2f != 0.3f`, so "is this key at the playhead"
 *     had no answer a keyer and a sampler were guaranteed to agree on. Integer ticks make that a `==`.
 *   - THE LOOP WRAP DRIFTED. `fmod` on a float accumulator leaves a residue per wrap; the clock below
 *     carries the leftover sub-tick explicitly, so the error is bounded by ONE tick (41 microseconds at
 *     24000) for as long as the process lives instead of growing with it.
 *
 * WHY 24000 TICKS PER SECOND. It is divisible by every display rate this engine can plausibly ship —
 * 24, 25, 30, 48, 50, 60, 100, 120 — so converting an imported clip's tick to project ticks is an exact
 * integer multiply and not a rounding. `Tests/Engine/AnimationTimeModel` asserts the divisibility rather
 * than trusting this paragraph. It is UE's default for the same reason
 * (`LevelSequenceProjectSettings.cpp:12`).
 *
 * THE TICK RATE AND THE DISPLAY RATE ARE TWO DIFFERENT NUMBERS AND THAT IS THE POINT. The tick rate is
 * the resolution time is STORED at; the display rate is the grid an artist SEES and snaps to. Conflating
 * them is what makes a 30 fps timeline unable to represent a 24 fps clip without resampling it.
 */

#include <cstdint>

namespace Desert::Animation
{
    /**
     * @brief An exact rational rate, in ticks or frames per second.
     *
     * A RATIONAL AND NOT A FLOAT, because the rates that matter are not representable: NTSC is 30000/1001
     * and 29.97 is not it. Storing the pair keeps `1001` frames taking exactly `1001 * 1000 / 30000`
     * seconds instead of accumulating the difference.
     */
    struct FrameRate
    {
        int32_t Numerator   = 60;
        int32_t Denominator = 1;

        [[nodiscard]] bool IsValid() const
        {
            return Numerator > 0 && Denominator > 0;
        }

        /// Ticks (or frames) per second as a real number. For arithmetic that has to end in a double —
        /// never for deciding whether two times are equal.
        [[nodiscard]] double AsDouble() const
        {
            return IsValid() ? static_cast<double>( Numerator ) / static_cast<double>( Denominator ) : 0.0;
        }

        [[nodiscard]] bool operator==( const FrameRate& other ) const
        {
            // VALUE EQUALITY, NOT FIELD EQUALITY: 30/1 and 60/2 are the same rate, and a caller asking
            // "are these the same grid" means the rate, not the spelling it arrived in.
            return static_cast<int64_t>( Numerator ) * other.Denominator ==
                   static_cast<int64_t>( other.Numerator ) * Denominator;
        }
        [[nodiscard]] bool operator!=( const FrameRate& other ) const
        {
            return !( *this == other );
        }
    };

    /// The project's storage resolution. Every `.anim` states its own (files are self-describing, so a
    /// future change is detectable rather than assumed), and this is what the importer and the authoring
    /// tools convert INTO.
    inline constexpr FrameRate PROJECT_TICK_RATE{ 24000, 1 };

    /// The display grid an artist gets unless a clip says otherwise. 30 rather than 24 because the engine's
    /// own fixed gameplay step is 1/60 and 30 divides it; a clip that wants film cadence states 24 itself.
    inline constexpr FrameRate DEFAULT_DISPLAY_RATE{ 30, 1 };

    /**
     * @brief A position on a tick grid: a whole number of ticks.
     *
     * A STRUCT AND NOT `using FrameNumber = int32_t`, so the compiler can tell a tick from a bone index,
     * from a key count, and from the float seconds this replaces. That is the entire reason it exists —
     * the arithmetic below is what an int already does.
     */
    struct FrameNumber
    {
        int32_t Value = 0;

        [[nodiscard]] bool operator==( const FrameNumber& o ) const
        {
            return Value == o.Value;
        }
        [[nodiscard]] bool operator!=( const FrameNumber& o ) const
        {
            return Value != o.Value;
        }
        [[nodiscard]] bool operator<( const FrameNumber& o ) const
        {
            return Value < o.Value;
        }
        [[nodiscard]] bool operator<=( const FrameNumber& o ) const
        {
            return Value <= o.Value;
        }
        [[nodiscard]] bool operator>( const FrameNumber& o ) const
        {
            return Value > o.Value;
        }
        [[nodiscard]] bool operator>=( const FrameNumber& o ) const
        {
            return Value >= o.Value;
        }
        [[nodiscard]] FrameNumber operator+( const FrameNumber& o ) const
        {
            return FrameNumber{ Value + o.Value };
        }
        [[nodiscard]] FrameNumber operator-( const FrameNumber& o ) const
        {
            return FrameNumber{ Value - o.Value };
        }
    };

    /**
     * @brief A tick plus where between it and the next one we are.
     *
     * The subframe exists because SAMPLING is continuous even when KEYING is not: a fixed 1/60 s gameplay
     * step lands 400 ticks apart at 24000, and the pose in between still has to be interpolated. It is
     * kept in [0, 1) by every function here, so it never grows into the float it is stored in.
     */
    struct FrameTime
    {
        FrameNumber Frame;
        float       Subframe = 0.0F;

        /// The position as a real number of ticks, for the interpolation factor a sampler needs.
        [[nodiscard]] double AsTicks() const
        {
            return static_cast<double>( Frame.Value ) + static_cast<double>( Subframe );
        }
    };

    /**
     * @brief What a tick-rate conversion did, INCLUDING whether it had to round.
     *
     * The honest answer to "what happened to precision at the importer boundary". assimp hands over key
     * times as doubles already counted in the exporter's own ticks (`aiAnimation::mTicksPerSecond`), and
     * the old importer narrowed each one to a float: exact for every integral tick below 2^24, and
     * silently not exact for a fractional one. Rounding is not the problem; an UNREPORTED rounding is.
     * `Exact` is false and `RoundedAwayTicks` says how far the result sits from the true value, in
     * destination ticks scaled by 1e6, so a caller can report micro-ticks instead of a ratio nobody reads.
     */
    struct TickConversion
    {
        FrameNumber Ticks;
        bool        Exact            = true;
        int64_t     RoundedAwayMicro = 0;
    };

    /// `tick`, counted at `from`, expressed at `to`. Exact whenever `to` is a whole multiple of `from`,
    /// which is why PROJECT_TICK_RATE is 24000 (see the file comment).
    [[nodiscard]] TickConversion ConvertTick( FrameNumber tick, FrameRate from, FrameRate to );

    /// Seconds -> a position on `rate`'s grid. The subframe carries the part that is not a whole tick.
    [[nodiscard]] FrameTime SecondsToFrameTime( double seconds, FrameRate rate );

    /// A position on `rate`'s grid -> seconds. Exact rational arithmetic, so a round trip through
    /// `SecondsToFrameTime` returns the same tick for every tick this engine can store.
    [[nodiscard]] double FrameTimeToSeconds( FrameTime time, FrameRate rate );

    /**
     * @brief The tick `time` is CLOSEST to, as opposed to the one it is inside.
     *
     * A TRAP WORTH A NAME, because it cost this suite a red run. `SecondsToFrameTime` floors — it splits a
     * continuous position into "which tick are we in" plus "how far through it", which is what a sampler
     * needs. But a `double` that came back through seconds is not the tick it started as: 799 ticks at
     * 24000 is 0.0332916..., and multiplying that back gives 798.9999999999999, which floors to 798. The
     * position is right to one part in 10^13; the FLOOR is off by a whole tick.
     *
     * So: ask which tick you are IN with `FrameTime::Frame`, and which tick you MEAN with this. Anything
     * that turns a mouse position, a scrub bar or a seconds value into "the frame the user is on" wants
     * this one.
     */
    [[nodiscard]] FrameNumber NearestTick( FrameTime time );

    /**
     * @brief Advance `time` by `seconds`, carrying the leftover sub-tick.
     *
     * THIS IS WHERE THE DRIFT USED TO LIVE. The float accumulator this replaces added `dt * tps` to a
     * growing float and wrapped it with `fmod`, so both the addition and the wrap lost a little every
     * frame and the loss grew with the number already there. Here the whole ticks go into an integer and
     * only the fraction of one tick stays in a float, so the error is bounded by a single tick forever.
     */
    [[nodiscard]] FrameTime AdvanceFrameTime( FrameTime time, double seconds, FrameRate rate );

    /**
     * @brief Wrap `time` into [0, duration) EXACTLY, the way a looping clip needs.
     *
     * Integer modulo on the whole ticks and the subframe carried through untouched. A negative position
     * wraps to the end rather than staying negative — playback can run backwards, and a clip scrubbed to
     * -1 tick is at its last tick, not before its first.
     */
    [[nodiscard]] FrameTime WrapFrameTime( FrameTime time, FrameNumber duration );

    /**
     * @brief The nearest position on the DISPLAY grid, expressed back in ticks.
     *
     * What makes the display rate a real field rather than a number in a file: the Sequencer's playhead
     * moves on this grid, so an artist lands ON frames and the key they author is at a tick two tools can
     * agree about. Returns `time`'s own tick unchanged when the two rates are the same grid.
     */
    [[nodiscard]] FrameNumber SnapToDisplayRate( FrameTime time, FrameRate tickRate, FrameRate displayRate );

    /// The display-frame index a tick falls on, for a ruler that shows frame numbers instead of seconds.
    [[nodiscard]] int32_t DisplayFrameIndex( FrameNumber tick, FrameRate tickRate, FrameRate displayRate );
} // namespace Desert::Animation
