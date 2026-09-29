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
        { return a.Name == b.Name && a.Tick.Value == b.Tick.Value && a.Track == b.Track; };
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
} // namespace Desert::Editor
