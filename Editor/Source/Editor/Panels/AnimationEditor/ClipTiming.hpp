#pragma once
#include <Editor/Panels/AnimationEditor/AnimationNotifyTracks.hpp> // EditClipSequence

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <array>
#include <functional>
#include <string_view>

namespace Desert::Editor
{
    /**
     * @brief The clip's Length and Display Rate as edits (UE: Anim Sequence ▸ Asset Details, Sequencer's
     *        Play Range and Display Rate), each ONE `SequenceEditCommand` through `EditClipSequence`.
     *
     * WHAT EACH EDIT MOVES, AND WHAT IT DOES NOT: keys are counted on `Sequence.TickRate`, so neither edit
     * touches a key. A new display rate changes the grid an artist snaps to and the frame numbers the
     * transport shows — a key at 0.5 s stays at 0.5 s. A new length moves the playback range's end
     * (`Sequence.End`, Start stays): shorter, and the keys past it stay in the sequence but the player
     * clamps and loops on the range (Sequence.hpp); longer, and the last key holds to the new end. Both are
     * fields the `.anim` already stores (SequenceFormat: DisplayRate, Start, End), so Save writes them.
     */

    /// The rates the Display Rate picker offers — UE's frame-rate menu: film, PAL, NTSC (exact rationals,
    /// TimeModel.hpp's reason) and the high rates.
    struct NamedFrameRate
    {
        std::string_view     Label;
        Animation::FrameRate Rate;
    };
    inline constexpr std::array<NamedFrameRate, 14> kClipDisplayRates{ {
         { "12 fps", { 12, 1 } },
         { "15 fps", { 15, 1 } },
         { "23.976 fps (NTSC film)", { 24000, 1001 } },
         { "24 fps (film)", { 24, 1 } },
         { "25 fps (PAL)", { 25, 1 } },
         { "29.97 fps (NTSC)", { 30000, 1001 } },
         { "30 fps", { 30, 1 } },
         { "48 fps", { 48, 1 } },
         { "50 fps (PAL)", { 50, 1 } },
         { "59.94 fps (NTSC)", { 60000, 1001 } },
         { "60 fps", { 60, 1 } },
         { "100 fps", { 100, 1 } },
         { "120 fps", { 120, 1 } },
         { "240 fps", { 240, 1 } },
    } };

    /**
     * @brief Set the clip's length to @p length ticks on its TickRate (the range becomes [Start, Start +
     *        length]) as ONE undo record.
     * @return false, and no record, for a length under one tick or no change.
     */
    inline bool SetClipLength( Animation::AnimationClip& clip, const Animation::FrameNumber length,
                               const std::function<void()>& changed )
    {
        if ( length.Value < 1 || length == clip.DurationTicks() )
            return false;
        return EditClipSequence( clip, changed,
                                 [length]( Animation::Timeline::Sequence& sequence )
                                 {
                                     sequence.End = Animation::FrameNumber{ sequence.Start.Value + length.Value };
                                     return true;
                                 } );
    }

    /// The length @p seconds asks for, on @p clip's tick grid: the NEAREST tick (TimeModel's NearestTick —
    /// 1.5 s typed must be 36000 ticks, not the 35999 a floor through a double can give).
    [[nodiscard]] inline Animation::FrameNumber ClipLengthTicks( const Animation::AnimationClip& clip,
                                                                 const double                    seconds )
    {
        return Animation::NearestTick( Animation::SecondsToFrameTime( seconds, clip.Sequence.TickRate ) );
    }

    /**
     * @brief Set the clip's display rate to @p rate as ONE undo record; no key moves (they are ticks).
     * @return false, and no record, for an invalid rate or the same rate (value equality: 60/2 is 30/1).
     */
    inline bool SetClipDisplayRate( Animation::AnimationClip& clip, const Animation::FrameRate rate,
                                    const std::function<void()>& changed )
    {
        if ( !rate.IsValid() || rate == clip.Sequence.DisplayRate )
            return false;
        return EditClipSequence( clip, changed,
                                 [rate]( Animation::Timeline::Sequence& sequence )
                                 {
                                     sequence.DisplayRate = rate;
                                     return true;
                                 } );
    }
} // namespace Desert::Editor
