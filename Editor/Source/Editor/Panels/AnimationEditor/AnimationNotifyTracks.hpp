#pragma once
#include <Editor/Core/Commands/SequenceEdit.hpp> // SameStoredValue, SequenceEditTransaction

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief The Animation Editor's Notifies and Curves tracks (UE's Notify Tracks, Anim Curves) as rules on
     *        the clip's ONE `Timeline::Sequence`.
     *
     * WHERE THEY LIVE (Timeline/Hosts.hpp, AnimationClip): the notifies are the Event keys of the clip's ONE
     * Event track on its Sequence binding (property ""), an `EventKey.Row` being the notify track a marker
     * stands on; each anim curve is a Float track on the same binding, named by its property. The notify
     * and curve lanes edit the track's FIRST section — the one a clip is lifted and created with; a section
     * layout of those tracks is the Sequencer's to author, the same way UE's Notify Tracks do not show it.
     *
     * RENDERER-FREE ON PURPOSE, like AnimationTransport: where a dragged notify lands, how many rows there
     * are, what one undo step restores and which marker lights up while the clip plays are rules a suite pins
     * here; the document only draws them.
     *
     * ONE EDIT, ONE UNDO RECORD, ONE COMMAND TYPE: every edit goes through `EditClipSequence`, which brackets
     * it in a `SequenceEditTransaction` — the `SequenceEditCommand` every other clip edit records (the
     * Sequencer's keys and sections, the pose). A drag is one edit on release, not one per pixel; an edit
     * that changes nothing records nothing (SequenceEditTransaction::End).
     */

    namespace NotifyTrackDetail
    {
        [[nodiscard]] inline const Animation::Timeline::Binding*
        SequenceBinding( const Animation::Timeline::Sequence& sequence )
        {
            for ( const auto& binding : sequence.Bindings )
                if ( binding.Kind == Animation::Timeline::BindingKind::Sequence )
                    return &binding;
            return nullptr;
        }

        [[nodiscard]] inline const Animation::Timeline::Track*
        FindSequenceTrack( const Animation::Timeline::Sequence& sequence, const Animation::Timeline::TrackKind kind,
                           const std::string_view property )
        {
            const auto* binding = SequenceBinding( sequence );
            if ( binding == nullptr )
                return nullptr;
            for ( const auto& track : sequence.Tracks )
                if ( track.Binding == binding->Guid && track.Kind == kind && track.Property == property )
                    return &track;
            return nullptr;
        }

        /// The track, its Sequence binding and its first (whole-clip) section created when missing. A
        /// structural edit bumps `Revision` (Sequence.hpp) — the Animator's binding cache is keyed by it.
        [[nodiscard]] inline Animation::Timeline::Section&
        EnsureFirstSection( Animation::Timeline::Sequence& sequence, const Animation::Timeline::TrackKind kind,
                            const std::string& property )
        {
            namespace T = Animation::Timeline;
            if ( SequenceBinding( sequence ) == nullptr )
            {
                T::Binding binding;
                binding.Guid = T::BindingGuid::Generate();
                binding.Kind = T::BindingKind::Sequence;
                sequence.Bindings.push_back( std::move( binding ) );
                ++sequence.Revision;
            }
            auto* found = const_cast<T::Track*>( FindSequenceTrack( sequence, kind, property ) );
            if ( found == nullptr )
            {
                T::Track track;
                track.Binding  = SequenceBinding( sequence )->Guid;
                track.Property = property;
                track.Kind     = kind;
                sequence.Tracks.push_back( std::move( track ) );
                ++sequence.Revision;
                found = &sequence.Tracks.back();
            }
            if ( found->Sections.empty() )
                (void)T::AddSection( *found, sequence.Start, sequence.End );
            return found->Sections.front();
        }

        template <typename ChannelType>
        [[nodiscard]] const ChannelType* FirstChannel( const Animation::Timeline::Track* track )
        {
            if ( track == nullptr || track->Sections.empty() )
                return nullptr;
            const auto* channel = std::get_if<Animation::Timeline::Channel>( &track->Sections.front().Content );
            return channel != nullptr ? std::get_if<ChannelType>( channel ) : nullptr;
        }

        template <typename ChannelType>
        [[nodiscard]] ChannelType& FirstChannelForEdit( Animation::Timeline::Sequence& sequence,
                                                        const Animation::Timeline::TrackKind kind,
                                                        const std::string& property )
        {
            auto& section = EnsureFirstSection( sequence, kind, property );
            auto* channel = std::get_if<Animation::Timeline::Channel>( &section.Content );
            if ( channel == nullptr || std::get_if<ChannelType>( channel ) == nullptr )
                section.Content = Animation::Timeline::Channel{ ChannelType{} };
            return std::get<ChannelType>( std::get<Animation::Timeline::Channel>( section.Content ) );
        }
    } // namespace NotifyTrackDetail

    /// The clip's Event channel (the notifies), or null when the clip has no notify track yet.
    [[nodiscard]] inline const Animation::Timeline::EventChannel* ClipNotifyChannel( const Animation::AnimationClip& clip )
    {
        return NotifyTrackDetail::FirstChannel<Animation::Timeline::EventChannel>( NotifyTrackDetail::FindSequenceTrack(
             clip.Sequence, Animation::Timeline::TrackKind::Event, "" ) );
    }

    /// The clip's notifies, in tick order (empty when it has none).
    [[nodiscard]] inline const std::vector<Animation::Timeline::EventKey>&
    ClipNotifies( const Animation::AnimationClip& clip )
    {
        static const std::vector<Animation::Timeline::EventKey> kNone;
        const auto* channel = ClipNotifyChannel( clip );
        return channel != nullptr ? channel->Keys : kNone;
    }

    /// The clip's anim curves: its Float tracks on the Sequence binding, in track order.
    [[nodiscard]] inline std::vector<const Animation::Timeline::Track*>
    ClipCurves( const Animation::AnimationClip& clip )
    {
        std::vector<const Animation::Timeline::Track*> out;
        const auto* binding = NotifyTrackDetail::SequenceBinding( clip.Sequence );
        if ( binding == nullptr )
            return out;
        for ( const auto& track : clip.Sequence.Tracks )
            if ( track.Binding == binding->Guid && track.Kind == Animation::Timeline::TrackKind::Float )
                out.push_back( &track );
        return out;
    }

    /// The keys of a curve track's first section (empty when it has none).
    [[nodiscard]] inline const std::vector<Animation::ScalarKey>& CurveKeys( const Animation::Timeline::Track& curve )
    {
        static const std::vector<Animation::ScalarKey> kNone;
        const auto* channel = NotifyTrackDetail::FirstChannel<Animation::Timeline::FloatChannel>( &curve );
        return channel != nullptr ? channel->Keys : kNone;
    }

    /// The curve's value at @p at, on the clip's tick rate.
    [[nodiscard]] inline float CurveValueAt( const Animation::Timeline::Track& curve, const Animation::FrameTime at,
                                             const Animation::FrameRate tickRate )
    {
        const auto* channel = NotifyTrackDetail::FirstChannel<Animation::Timeline::FloatChannel>( &curve );
        return channel != nullptr ? Animation::Timeline::Evaluate( *channel, at, tickRate ) : 0.0F;
    }

    /**
     * @brief Run @p edit on @p clip's sequence as ONE `SequenceEditCommand` in CommandHistory::Get().
     *
     * @p edit returns false to refuse — and must then not have touched the sequence (a refused edit records
     * nothing and restores nothing). @p changed runs after the edit and after every undo/redo of it (the
     * owner's AfterRestore). @return whether an undo record was pushed.
     */
    template <typename Edit>
    bool EditClipSequence( Animation::AnimationClip& clip, const std::function<void()>& changed, Edit&& edit )
    {
        SequenceOwner owner = OwnerOf( &clip );
        owner.AfterRestore  = changed;
        SequenceEditTransaction transaction;
        if ( const auto began = transaction.Begin( std::move( owner ) ); !began.IsSuccess() )
            return false;
        if ( !edit( clip.Sequence ) )
        {
            transaction.Cancel();
            return false;
        }
        const auto ended    = transaction.End();
        const bool recorded = ended.IsSuccess() && ended.GetValue() > 0;
        if ( recorded && changed )
            changed();
        return recorded;
    }

    // Rows to draw: every row a notify stands on, the rows the person added and left empty, at least one.
    [[nodiscard]] inline int32_t NotifyTrackCount( const std::vector<Animation::Timeline::EventKey>& notifies,
                                                   const int32_t                                     addedRows )
    {
        int32_t rows = std::max( addedRows, 1 );
        for ( const auto& notify : notifies )
            rows = std::max( rows, notify.Row + 1 );
        return rows;
    }

    // The tick a notify dropped at @p seconds lands on: the clip's DISPLAY grid (the frame an artist sees on the
    // ruler), inside the clip.
    [[nodiscard]] inline Animation::FrameNumber SnapNotifyTick( const double                 seconds,
                                                                const Animation::FrameRate   tickRate,
                                                                const Animation::FrameRate   displayRate,
                                                                const Animation::FrameNumber durationTicks )
    {
        const Animation::FrameNumber snapped = Animation::SnapToDisplayRate(
             Animation::SecondsToFrameTime( std::max( seconds, 0.0 ), tickRate ), tickRate, displayRate );
        return Animation::FrameNumber{ std::clamp( snapped.Value, 0, std::max( durationTicks.Value, 0 ) ) };
    }

    // An Event channel is sorted by tick (Channel.hpp); an edit keeps that order. Stable, so two notifies on one
    // tick keep the order the person made them in (they fire in list order).
    inline void SortNotifies( std::vector<Animation::Timeline::EventKey>& notifies )
    {
        std::ranges::stable_sort( notifies, {}, []( const Animation::Timeline::EventKey& n ) { return n.Tick.Value; } );
    }

    // The notifies to light this frame: those the playhead crossed moving @p before -> @p after (seconds). A
    // backwards move that is not a loop wrap (a scrub to the left) crosses nothing, like playback, which never
    // fires backwards. The rule is the Animator's (Timeline::CollectCrossed); a state lights when it begins.
    [[nodiscard]] inline std::vector<size_t> CrossedNotifies( const Animation::AnimationClip& clip,
                                                              const double before, const double after,
                                                              const bool looped )
    {
        std::vector<size_t> out;
        const auto*         channel = ClipNotifyChannel( clip );
        if ( channel == nullptr || ( !looped && !( after > before ) ) )
            return out;
        const Animation::FrameRate                     rate = clip.Sequence.TickRate;
        std::vector<Animation::Timeline::CrossedEvent> crossed;
        Animation::Timeline::CollectCrossed( *channel, Animation::SecondsToFrameTime( before, rate ),
                                             Animation::SecondsToFrameTime( after, rate ), looped,
                                             clip.Sequence.Start, clip.Sequence.End, crossed );
        for ( const auto& event : crossed )
            if ( event.Key != nullptr && event.Edge != Animation::Timeline::EventEdge::End )
                out.push_back( static_cast<size_t>( event.Key - channel->Keys.data() ) );
        return out;
    }

    /// Two notify lists are the same authoring: name, tick, row AND length (a Notify State's span).
    [[nodiscard]] inline bool SameNotifies( const std::vector<Animation::Timeline::EventKey>& a,
                                            const std::vector<Animation::Timeline::EventKey>& b )
    {
        return std::ranges::equal( a, b, []( const auto& x, const auto& y ) { return SameStoredValue( x, y ); } );
    }

    /**
     * @brief Replace @p clip's notifies with @p edited (sorted here) as ONE undo record.
     * @return false, and no record, when the edit changes nothing.
     */
    inline bool ApplyNotifyEdit( Animation::AnimationClip& clip, std::vector<Animation::Timeline::EventKey> edited,
                                 const std::function<void()>& changed )
    {
        SortNotifies( edited );
        if ( SameNotifies( edited, ClipNotifies( clip ) ) )
            return false;
        return EditClipSequence( clip, changed,
                                 [&edited]( Animation::Timeline::Sequence& sequence )
                                 {
                                     NotifyTrackDetail::FirstChannelForEdit<Animation::Timeline::EventChannel>(
                                          sequence, Animation::Timeline::TrackKind::Event, "" )
                                          .Keys = std::move( edited );
                                     return true;
                                 } );
    }

    /**
     * @brief Give notify @p index a length (UE: a Notify State) or take it away (0 = instant), as ONE undo
     *        record. A drag of a state's end handle is one call on release, like a move.
     * @return false, and no record, for an index out of range, a negative length or no change.
     */
    inline bool SetNotifyDuration( Animation::AnimationClip& clip, const size_t index,
                                   const Animation::FrameNumber duration, const std::function<void()>& changed )
    {
        const auto& notifies = ClipNotifies( clip );
        if ( index >= notifies.size() || duration.Value < 0 )
            return false;
        std::vector<Animation::Timeline::EventKey> edited = notifies;
        edited[index].Duration                            = duration;
        return ApplyNotifyEdit( clip, std::move( edited ), changed );
    }

    /**
     * @brief Whether the Animation Editor's clip differs from what its FILE holds: the rule behind "Save*".
     *
     * The comparison is against a clip read from the file, never against a snapshot the window took of the
     * asset: the asset outlives the window, so a snapshot taken at reopen would call an unsaved edit clean
     * (ANV1c3: close with Save*, reopen, "Save" — and the edit still in memory). Everything the editor can
     * author lives in the sequence (notifies, curves, bone keys, sections), so the sequence is the answer.
     */
    [[nodiscard]] inline bool ClipDiffersFromFile( const Animation::AnimationClip& inMemory,
                                                   const Animation::AnimationClip& onDisk )
    {
        return !SameStoredValue( inMemory.Sequence, onDisk.Sequence );
    }

    /// Which edge of a Notify State's bar a drag holds.
    enum class NotifyStateEdge : uint8_t
    {
        Begin,
        End
    };

    /**
     * @brief Where a drag of a Notify State's @p edge to @p tick leaves the notify (UE: dragging a state's
     *        ends). The opposite edge stays put; the span keeps at least one tick and stays inside
     *        [0, @p clipDuration]. The document commits the result as ONE edit on release.
     */
    [[nodiscard]] inline Animation::Timeline::EventKey
    DragNotifyStateEdge( Animation::Timeline::EventKey notify, const NotifyStateEdge edge,
                         const Animation::FrameNumber tick, const Animation::FrameNumber clipDuration )
    {
        const int64_t begin = notify.Tick.Value;
        const int64_t end   = begin + std::max<int64_t>( notify.Duration.Value, 1 );
        if ( edge == NotifyStateEdge::End )
        {
            const int64_t newEnd =
                 std::clamp<int64_t>( tick.Value, begin + 1, std::max<int64_t>( clipDuration.Value, begin + 1 ) );
            notify.Duration.Value = static_cast<decltype( notify.Duration.Value )>( newEnd - begin );
        }
        else
        {
            const int64_t newBegin     = std::clamp<int64_t>( tick.Value, 0, end - 1 );
            notify.Tick.Value          = static_cast<decltype( notify.Tick.Value )>( newBegin );
            notify.Duration.Value = static_cast<decltype( notify.Duration.Value )>( end - newBegin );
        }
        return notify;
    }

    /**
     * @brief Key anim curve @p name at @p tick (replacing a key already on that tick; creating the curve
     *        when the clip has none of that name), as ONE undo record. Keys stay in tick order and the Auto
     *        tangents are recomputed, so every edit leaves a curve the Animator can sample.
     */
    inline bool SetCurveKey( Animation::AnimationClip& clip, const std::string& name,
                             const Animation::FrameNumber tick, const float value,
                             const Animation::KeyInterp interp, const std::function<void()>& changed )
    {
        if ( name.empty() )
            return false;
        return EditClipSequence(
             clip, changed,
             [&]( Animation::Timeline::Sequence& sequence )
             {
                 auto& keys = NotifyTrackDetail::FirstChannelForEdit<Animation::Timeline::FloatChannel>(
                                   sequence, Animation::Timeline::TrackKind::Float, name )
                                   .Keys;
                 auto key = std::ranges::find( keys, tick, &Animation::ScalarKey::Tick );
                 if ( key == keys.end() )
                 {
                     Animation::ScalarKey added;
                     added.Tick = tick;
                     keys.push_back( added );
                     key = keys.end() - 1;
                 }
                 key->Value  = value;
                 key->Interp = interp;
                 std::ranges::stable_sort( keys, {}, &Animation::ScalarKey::Tick );
                 Animation::AutoSetTangents( keys, sequence.TickRate );
                 return true;
             } );
    }

    /// Move the key of anim curve @p name at @p from to @p to (a key already on @p to is replaced), as ONE
    /// undo record; false when there is no such key or the move goes nowhere.
    inline bool MoveCurveKey( Animation::AnimationClip& clip, const std::string& name,
                              const Animation::FrameNumber from, const Animation::FrameNumber to,
                              const std::function<void()>& changed )
    {
        const auto* track =
             NotifyTrackDetail::FindSequenceTrack( clip.Sequence, Animation::Timeline::TrackKind::Float, name );
        if ( track == nullptr || from == to ||
             std::ranges::find( CurveKeys( *track ), from, &Animation::ScalarKey::Tick ) == CurveKeys( *track ).end() )
            return false;
        return EditClipSequence( clip, changed,
                                 [&]( Animation::Timeline::Sequence& sequence )
                                 {
                                     auto& keys =
                                          NotifyTrackDetail::FirstChannelForEdit<Animation::Timeline::FloatChannel>(
                                               sequence, Animation::Timeline::TrackKind::Float, name )
                                               .Keys;
                                     auto moved = *std::ranges::find( keys, from, &Animation::ScalarKey::Tick );
                                     std::erase_if( keys, [&]( const Animation::ScalarKey& k )
                                                    { return k.Tick == from || k.Tick == to; } );
                                     moved.Tick = to;
                                     keys.push_back( moved );
                                     std::ranges::stable_sort( keys, {}, &Animation::ScalarKey::Tick );
                                     Animation::AutoSetTangents( keys, sequence.TickRate );
                                     return true;
                                 } );
    }

    /// Remove the key of anim curve @p name at @p tick, as ONE undo record; false when there is none.
    inline bool RemoveCurveKey( Animation::AnimationClip& clip, const std::string& name,
                                const Animation::FrameNumber tick, const std::function<void()>& changed )
    {
        const auto* track =
             NotifyTrackDetail::FindSequenceTrack( clip.Sequence, Animation::Timeline::TrackKind::Float, name );
        if ( track == nullptr ||
             std::ranges::find( CurveKeys( *track ), tick, &Animation::ScalarKey::Tick ) == CurveKeys( *track ).end() )
            return false;
        return EditClipSequence( clip, changed,
                                 [&]( Animation::Timeline::Sequence& sequence )
                                 {
                                     auto& keys =
                                          NotifyTrackDetail::FirstChannelForEdit<Animation::Timeline::FloatChannel>(
                                               sequence, Animation::Timeline::TrackKind::Float, name )
                                               .Keys;
                                     std::erase_if( keys,
                                                    [tick]( const Animation::ScalarKey& k ) { return k.Tick == tick; } );
                                     Animation::AutoSetTangents( keys, sequence.TickRate );
                                     return true;
                                 } );
    }
} // namespace Desert::Editor
