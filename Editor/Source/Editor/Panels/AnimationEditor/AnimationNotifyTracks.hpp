#pragma once

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief The Animation Editor's Notifies tracks (UE's Notify Tracks) as rules on a clip's notify list.
     *
     * RENDERER-FREE ON PURPOSE, like AnimationTransport: where a dragged notify lands, how many rows there are,
     * what one undo step restores and which marker lights up while the clip plays are rules a suite pins here;
     * the document only draws them.
     *
     * ONE EDIT, ONE UNDO RECORD: every edit (add, move, rename, delete) replaces the whole list through
     * ApplyNotifyEdit, and the record holds the list before and after. A drag is one edit — the document
     * calls it on release, not per frame — so a move is undone by one Ctrl+Z, not by one per pixel.
     */

    // Rows to draw: every track a notify stands on, the rows the person added and left empty, at least one.
    [[nodiscard]] inline int32_t NotifyTrackCount( const std::vector<Animation::AnimationNotify>& notifies,
                                                   const int32_t                                  addedRows )
    {
        int32_t rows = std::max( addedRows, 1 );
        for ( const auto& notify : notifies )
            rows = std::max( rows, notify.Track + 1 );
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

    // The Animator scans notifies in tick order (AnimationClipBuild sorts on load); an edit keeps that order.
    // Stable, so two notifies on one tick keep the order the person made them in.
    inline void SortNotifies( std::vector<Animation::AnimationNotify>& notifies )
    {
        std::ranges::stable_sort( notifies, {},
                                  []( const Animation::AnimationNotify& n ) { return n.Tick.Value; } );
    }

    // The notifies to light this frame: those the playhead crossed moving @p before -> @p after (seconds). A
    // backwards move that is not a loop wrap (a scrub to the left) crosses nothing, like playback, which never
    // fires backwards. Same rule as the Animator (Animation::NotifyCrossed).
    [[nodiscard]] inline std::vector<size_t>
    CrossedNotifies( const std::vector<Animation::AnimationNotify>& notifies, const Animation::FrameRate tickRate,
                     const double before, const double after, const bool looped )
    {
        std::vector<size_t> out;
        if ( !looped && !( after > before ) )
            return out;
        const double from = Animation::SecondsToFrameTime( before, tickRate ).AsTicks();
        const double to   = Animation::SecondsToFrameTime( after, tickRate ).AsTicks();
        for ( size_t i = 0; i < notifies.size(); ++i )
            if ( Animation::NotifyCrossed( static_cast<double>( notifies[i].Tick.Value ), from, to, looped ) )
                out.push_back( i );
        return out;
    }

    /// One undo record for a notify edit: the whole list before and after, and the hook that tells the owner
    /// the clip changed (the document marks itself unsaved).
    class NotifyEditCommand final : public ICommand
    {
    public:
        NotifyEditCommand( Animation::AnimationClip* clip, std::vector<Animation::AnimationNotify> before,
                           std::vector<Animation::AnimationNotify> after, std::string label,
                           std::function<void()> changed )
             : m_Clip( clip ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
               m_Label( std::move( label ) ), m_Changed( std::move( changed ) )
        {
        }

        bool Undo() override
        {
            return Set( m_Before );
        }
        bool Redo() override
        {
            return Set( m_After );
        }
        [[nodiscard]] std::string GetLabel() const override
        {
            return m_Label;
        }

    private:
        bool Set( const std::vector<Animation::AnimationNotify>& notifies )
        {
            if ( m_Clip == nullptr )
                return false;
            m_Clip->Notifies = notifies;
            if ( m_Changed )
                m_Changed();
            return true;
        }

        Animation::AnimationClip*               m_Clip;
        std::vector<Animation::AnimationNotify> m_Before;
        std::vector<Animation::AnimationNotify> m_After;
        std::string                             m_Label;
        std::function<void()>                   m_Changed;
    };

    /**
     * @brief Replace @p clip's notifies with @p edited (sorted here) as ONE undo record in @p history.
     * @return false, and no record, when the edit changes nothing.
     */
    inline bool ApplyNotifyEdit( Animation::AnimationClip& clip, std::vector<Animation::AnimationNotify> edited,
                                 std::string label, CommandHistory& history, const std::function<void()>& changed )
    {
        SortNotifies( edited );
        const auto same = []( const Animation::AnimationNotify& a, const Animation::AnimationNotify& b )
        {
            return a.Name == b.Name && a.Tick.Value == b.Tick.Value && a.Track == b.Track &&
                   a.DurationTicks.Value == b.DurationTicks.Value;
        };
        if ( std::ranges::equal( edited, clip.Notifies, same ) )
            return false;
        std::vector<Animation::AnimationNotify> before = clip.Notifies;
        clip.Notifies                                  = edited;
        history.PushCommand( std::make_unique<NotifyEditCommand>( &clip, std::move( before ), std::move( edited ),
                                                                  std::move( label ), changed ) );
        if ( changed )
            changed();
        return true;
    }

    /**
     * @brief Give notify @p index a length (UE: a Notify State) or take it away (0 = instant), as ONE undo
     *        record. A drag of a state's end handle is one call on release, like a move.
     * @return false, and no record, for an index out of range, a negative length or no change.
     */
    inline bool SetNotifyDuration( Animation::AnimationClip& clip, const size_t index,
                                   const Animation::FrameNumber duration, CommandHistory& history,
                                   const std::function<void()>& changed )
    {
        if ( index >= clip.Notifies.size() || duration.Value < 0 )
            return false;
        std::vector<Animation::AnimationNotify> edited = clip.Notifies;
        edited[index].DurationTicks                    = duration;
        return ApplyNotifyEdit( clip, std::move( edited ), "Set Notify Duration", history, changed );
    }

    /// Two curve lists hold the same authored keys (name, tick, value, interpolation, tangents).
    [[nodiscard]] inline bool SameCurves( const std::vector<Animation::AnimationCurve>& a,
                                          const std::vector<Animation::AnimationCurve>& b )
    {
        const auto sameKey = []( const Animation::ScalarKey& x, const Animation::ScalarKey& y )
        {
            return x.Tick == y.Tick && x.Value == y.Value && x.Interp == y.Interp && x.Mode == y.Mode &&
                   x.ArriveTangent == y.ArriveTangent && x.LeaveTangent == y.LeaveTangent;
        };
        return std::ranges::equal( a, b,
                                   [&sameKey]( const Animation::AnimationCurve& x, const Animation::AnimationCurve& y )
                                   { return x.Name == y.Name && std::ranges::equal( x.Keys, y.Keys, sameKey ); } );
    }

    /// Two notify lists are the same authoring: name, tick, track AND length (a Notify State's span).
    [[nodiscard]] inline bool SameNotifies( const std::vector<Animation::AnimationNotify>& a,
                                            const std::vector<Animation::AnimationNotify>& b )
    {
        return std::ranges::equal( a, b,
                                   []( const Animation::AnimationNotify& x, const Animation::AnimationNotify& y )
                                   {
                                       return x.Name == y.Name && x.Tick.Value == y.Tick.Value &&
                                              x.Track == y.Track && x.DurationTicks.Value == y.DurationTicks.Value;
                                   } );
    }

    /**
     * @brief Whether the Animation Editor's clip differs from what its FILE holds: the rule behind "Save*".
     *
     * The comparison is against a clip read from the file, never against a snapshot the window took of the
     * asset: the asset outlives the window, so a snapshot taken at reopen would call an unsaved edit clean
     * (ANV1c3: close with Save*, reopen, "Save" — and the edit still in memory). Everything the editor can
     * author counts: notifies (with their state length) and anim curves.
     */
    [[nodiscard]] inline bool ClipDiffersFromFile( const Animation::AnimationClip& inMemory,
                                                   const Animation::AnimationClip& onDisk )
    {
        return !SameNotifies( inMemory.Notifies, onDisk.Notifies ) || !SameCurves( inMemory.Curves, onDisk.Curves );
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
    [[nodiscard]] inline Animation::AnimationNotify DragNotifyStateEdge( Animation::AnimationNotify     notify,
                                                                         const NotifyStateEdge          edge,
                                                                         const Animation::FrameNumber   tick,
                                                                         const Animation::FrameNumber   clipDuration )
    {
        const int64_t begin = notify.Tick.Value;
        const int64_t end   = begin + std::max<int64_t>( notify.DurationTicks.Value, 1 );
        if ( edge == NotifyStateEdge::End )
        {
            const int64_t newEnd  = std::clamp<int64_t>( tick.Value, begin + 1, std::max<int64_t>( clipDuration.Value, begin + 1 ) );
            notify.DurationTicks.Value = static_cast<decltype( notify.DurationTicks.Value )>( newEnd - begin );
        }
        else
        {
            const int64_t newBegin     = std::clamp<int64_t>( tick.Value, 0, end - 1 );
            notify.Tick.Value          = static_cast<decltype( notify.Tick.Value )>( newBegin );
            notify.DurationTicks.Value = static_cast<decltype( notify.DurationTicks.Value )>( end - newBegin );
        }
        return notify;
    }

    /// One undo record for an anim-curve edit: the clip's whole curve list before and after.
    class CurveEditCommand final : public ICommand
    {
    public:
        CurveEditCommand( Animation::AnimationClip* clip, std::vector<Animation::AnimationCurve> before,
                          std::vector<Animation::AnimationCurve> after, std::string label,
                          std::function<void()> changed )
             : m_Clip( clip ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
               m_Label( std::move( label ) ), m_Changed( std::move( changed ) )
        {
        }

        bool Undo() override
        {
            return Set( m_Before );
        }
        bool Redo() override
        {
            return Set( m_After );
        }
        [[nodiscard]] std::string GetLabel() const override
        {
            return m_Label;
        }

    private:
        bool Set( const std::vector<Animation::AnimationCurve>& curves )
        {
            if ( m_Clip == nullptr )
                return false;
            m_Clip->Curves = curves;
            if ( m_Changed )
                m_Changed();
            return true;
        }

        Animation::AnimationClip*              m_Clip;
        std::vector<Animation::AnimationCurve> m_Before;
        std::vector<Animation::AnimationCurve> m_After;
        std::string                            m_Label;
        std::function<void()>                  m_Changed;
    };

    /**
     * @brief Replace @p clip's anim curves with @p edited as ONE undo record. Keys are sorted by tick and
     *        the Auto tangents recomputed here, so every edit leaves a curve the Animator can sample.
     * @return false, and no record, when the edit changes nothing.
     */
    inline bool ApplyCurveEdit( Animation::AnimationClip& clip, std::vector<Animation::AnimationCurve> edited,
                                std::string label, CommandHistory& history, const std::function<void()>& changed )
    {
        for ( auto& curve : edited )
        {
            std::ranges::stable_sort( curve.Keys, {}, []( const Animation::ScalarKey& k ) { return k.Tick; } );
            Animation::AutoSetTangents( curve.Keys, clip.TickRate );
        }
        if ( SameCurves( edited, clip.Curves ) )
            return false;
        std::vector<Animation::AnimationCurve> before = clip.Curves;
        clip.Curves                                   = edited;
        history.PushCommand( std::make_unique<CurveEditCommand>( &clip, std::move( before ), std::move( edited ),
                                                                 std::move( label ), changed ) );
        if ( changed )
            changed();
        return true;
    }

    /**
     * @brief Key anim curve @p name at @p tick (replacing a key already on that tick; creating the curve
     *        when the clip has none of that name), as ONE undo record.
     */
    inline bool SetCurveKey( Animation::AnimationClip& clip, const std::string& name,
                             const Animation::FrameNumber tick, const float value,
                             const Animation::KeyInterp interp, CommandHistory& history,
                             const std::function<void()>& changed )
    {
        if ( name.empty() )
            return false;
        std::vector<Animation::AnimationCurve> edited = clip.Curves;
        auto curve = std::ranges::find( edited, name, &Animation::AnimationCurve::Name );
        if ( curve == edited.end() )
        {
            edited.push_back( Animation::AnimationCurve{ name, {} } );
            curve = edited.end() - 1;
        }
        auto key = std::ranges::find( curve->Keys, tick, &Animation::ScalarKey::Tick );
        if ( key == curve->Keys.end() )
        {
            Animation::ScalarKey added;
            added.Tick = tick;
            curve->Keys.push_back( added );
            key = curve->Keys.end() - 1;
        }
        key->Value  = value;
        key->Interp = interp;
        return ApplyCurveEdit( clip, std::move( edited ), "Set Curve Key", history, changed );
    }

    /// Remove the key of anim curve @p name at @p tick, as ONE undo record; false when there is none.
    inline bool RemoveCurveKey( Animation::AnimationClip& clip, const std::string& name,
                                const Animation::FrameNumber tick, CommandHistory& history,
                                const std::function<void()>& changed )
    {
        std::vector<Animation::AnimationCurve> edited = clip.Curves;
        const auto curve = std::ranges::find( edited, name, &Animation::AnimationCurve::Name );
        if ( curve == edited.end() || std::erase_if( curve->Keys, [tick]( const Animation::ScalarKey& k )
                                                     { return k.Tick == tick; } ) == 0 )
            return false;
        return ApplyCurveEdit( clip, std::move( edited ), "Remove Curve Key", history, changed );
    }
} // namespace Desert::Editor
