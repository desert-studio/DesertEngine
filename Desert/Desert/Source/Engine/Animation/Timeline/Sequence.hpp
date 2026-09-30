#pragma once

/**
 * A SEQUENCE: THE DATA. NO TIME, NO PLAYBACK STATE, NO LIVE OBJECTS.
 *
 * Everything a file stores and nothing a frame computes. The `Player` (Player.hpp) holds where playback
 * is; the `Evaluator` (Evaluator.hpp) turns a time into values; the HOST owns the sequence and says which
 * objects its bindings mean. That split is UE's (UMovieScene / UMovieSceneSequencePlayer / the evaluation
 * template / IMovieScenePlayer) and it is what lets one Sequence be played by two players at once — an
 * editor preview beside the game, a crowd of characters on one clip — without either writing into it.
 *
 * ── THE HOST IS PART OF THE DATA ──────────────────────────────────────────────────────────────────────
 *
 * `Host` restricts what a sequence may contain, and `Validate` enforces it, because a `.anim` that grew a
 * Camera Cut track would be a file no consumer of `.anim` can play:
 *
 *     AnimationClip   Bone bindings (Transform tracks) + Sequence bindings (Float curves, one Event track)
 *     UIAnimation     Widget bindings, Vector/Float tracks; one widget = the element that owns the clip
 *     LevelSequence   anything: Entity bindings (+ Bone under Entity), Animation, CameraCut, Event
 *
 * ── SERIALIZATION ─────────────────────────────────────────────────────────────────────────────────────
 *
 * One binary block, subsystem `TMLN` version `kTimelineFormatVersion`, carried inside each host's asset
 * envelope (`.anim` → its AnimationAssetData, UI → the widget asset, `.dseq` → its own envelope). ONE
 * writer and ONE reader for all three hosts: a format a host writes by hand is a second format. Integers
 * for every enum (their orders are append-only), GUIDs as 128 bits, keys as the ScalarKey fields in
 * declaration order including the reserved weights.
 */

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Track.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Animation::Timeline
{
    /// STORED AS AN INTEGER: append only.
    enum class SequenceHost : uint8_t
    {
        AnimationClip = 0,
        UIAnimation   = 1,
        LevelSequence = 2,
    };

    [[nodiscard]] const char* ToString( SequenceHost host );

    inline constexpr uint32_t kTimelineFormatVersion = 1;

    struct Sequence
    {
        SequenceHost Host = SequenceHost::LevelSequence;

        /// Keys are counted on `TickRate`; an artist edits on `DisplayRate` (TimeModel.hpp's two numbers).
        FrameRate TickRate    = PROJECT_TICK_RATE;
        FrameRate DisplayRate = DEFAULT_DISPLAY_RATE;

        /// The playback range, inclusive, in ticks. A player loops and clamps on THIS, not on the keys.
        FrameNumber Start;
        FrameNumber End;

        std::vector<Binding> Bindings;
        std::vector<Track>   Tracks;

        /**
         * @brief Bumped by every structural edit (a binding or a track added, removed, re-bound).
         *
         * The Evaluator's resolved-binding cache is keyed by it — AnimationClip::TrackRevision's lesson:
         * a vector's address is not an identity. NOT SERIALIZED: it describes this process's copy.
         */
        uint32_t Revision = 0;
    };

    [[nodiscard]] const Binding* FindBinding( const Sequence& sequence, const BindingGuid& guid );
    [[nodiscard]] const Track*   FindTrack( const Sequence& sequence, const BindingGuid& binding,
                                            std::string_view property );

    /**
     * @brief Every invariant stated in this folder, checked. The FIRST violation, named.
     *
     * The error names the track (binding label + property) and the section index, e.g.
     * "track 'Hand_L' / '' section 2: rotation keys are not aligned (X has 4 ticks, W has 3)". Readers call
     * it after `ReadSequence`; editors call it before save. Nothing downstream re-checks.
     */
    [[nodiscard]] Common::BoolResultStr Validate( const Sequence& sequence );

    [[nodiscard]] std::vector<uint8_t> WriteSequence( const Sequence& sequence );
    /// Refuses a newer version than this build knows, and bytes that end early, by name. Validates.
    [[nodiscard]] Common::ResultStr<Sequence> ReadSequence( std::span<const uint8_t> bytes );
} // namespace Desert::Animation::Timeline
