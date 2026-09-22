#include "UIClipEdit.hpp"

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <utility>

namespace Desert::Editor
{
    bool SameStoredValue( const ECS::UIAnimKey& a, const ECS::UIAnimKey& b )
    {
        const auto& [aTime, aValue, aEasing] = a;
        const auto& [bTime, bValue, bEasing] = b;
        return aTime == bTime && aValue == bValue && aEasing == bEasing;
    }

    bool SameStoredValue( const ECS::UIAnimTrack& a, const ECS::UIAnimTrack& b )
    {
        const auto& [aProperty, aKeys] = a;
        const auto& [bProperty, bKeys] = b;
        if ( aProperty != bProperty || aKeys.size() != bKeys.size() )
        {
            return false;
        }
        for ( size_t i = 0; i < aKeys.size(); ++i )
        {
            if ( !SameStoredValue( aKeys[i], bKeys[i] ) )
            {
                return false;
            }
        }
        return true;
    }

    UIClipContent CaptureUIClip( const ECS::UIAnimData& clip )
    {
        // THE BINDING IS THE CENSUS. Naming all five members and then using three is the visible form of
        // "the playhead and the transport are deliberately not snapshotted" — a sixth field cannot be
        // added to UIAnimData without this line failing to compile and asking which half it belongs to.
        const auto& [tracks, duration, loop, playing, time] = clip;
        (void)playing;
        (void)time;

        UIClipContent content;
        content.Tracks   = tracks;
        content.Duration = duration;
        content.Loop     = loop;
        return content;
    }

    void RestoreUIClip( ECS::UIAnimData& clip, const UIClipContent& content )
    {
        clip.Tracks   = content.Tracks;
        clip.Duration = content.Duration;
        clip.Loop     = content.Loop;
    }

    bool SameStoredValue( const UIClipContent& a, const UIClipContent& b )
    {
        if ( a.Duration != b.Duration || a.Loop != b.Loop || a.Tracks.size() != b.Tracks.size() )
        {
            return false;
        }
        for ( size_t i = 0; i < a.Tracks.size(); ++i )
        {
            if ( !SameStoredValue( a.Tracks[i], b.Tracks[i] ) )
            {
                return false;
            }
        }
        return true;
    }

    // ── UIClipCommand ────────────────────────────────────────────────────────────────────────────────

    UIClipCommand::UIClipCommand( ECS::UIAnimData* clip, UIClipContent before, UIClipContent after )
         : m_Clip( clip ), m_Before( std::move( before ) ), m_After( std::move( after ) )
    {
    }

    bool UIClipCommand::Undo()
    {
        return Apply( m_Before );
    }

    bool UIClipCommand::Redo()
    {
        return Apply( m_After );
    }

    bool UIClipCommand::Apply( const UIClipContent& state )
    {
        if ( m_Clip == nullptr )
        {
            // CommandHistory::Undo discards an entry that reports failure and keeps walking down, which is
            // the only sane answer for an entry whose subject is gone.
            LOG_ERROR( "[UIClipUndo] the clip this entry was recorded against is gone; the entry is dropped" );
            return false;
        }
        RestoreUIClip( *m_Clip, state );
        return true;
    }

    std::string UIClipCommand::GetLabel() const
    {
        return "UI animation edit";
    }

    size_t UIClipCommand::ChangedTracks() const
    {
        const size_t maxTracks = std::max( m_Before.Tracks.size(), m_After.Tracks.size() );
        size_t       changed   = 0;
        for ( size_t i = 0; i < maxTracks; ++i )
        {
            const bool hasBefore = i < m_Before.Tracks.size();
            const bool hasAfter  = i < m_After.Tracks.size();
            if ( hasBefore && hasAfter && SameStoredValue( m_Before.Tracks[i], m_After.Tracks[i] ) )
            {
                continue;
            }
            ++changed;
        }
        return changed;
    }

    // ── UIClipEditTransaction ────────────────────────────────────────────────────────────────────────

    Common::BoolResultStr UIClipEditTransaction::Begin( ECS::UIAnimData* clip )
    {
        if ( m_Open )
        {
            return Common::MakeFormattedError<bool>(
                 "a UI clip transaction is already open ({} lane(s) captured); they do not nest, because "
                 "there is no answer to which end commits the undo entry",
                 m_Before.Tracks.size() );
        }
        if ( clip == nullptr )
        {
            return Common::MakeFormattedError<bool>(
                 "a UI clip transaction needs a clip; an entry with no subject would restore nothing and "
                 "still occupy a Ctrl+Z" );
        }

        m_Clip   = clip;
        m_Open   = true;
        m_Before = CaptureUIClip( *clip );
        return Common::MakeSuccess( true );
    }

    void UIClipEditTransaction::Cancel()
    {
        m_Open = false;
        m_Clip = nullptr;
        m_Before.Tracks.clear();
    }

    Common::ResultStr<uint32_t> UIClipEditTransaction::End()
    {
        if ( !m_Open )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "no UI clip transaction is open; an entry closed by an end that never began would hold "
                 "whatever the last interaction left behind" );
        }
        if ( m_Clip == nullptr )
        {
            Cancel();
            return Common::MakeFormattedError<uint32_t>(
                 "the transaction's clip went away while it was open; nothing was recorded" );
        }

        UIClipContent after = CaptureUIClip( *m_Clip );
        if ( SameStoredValue( m_Before, after ) )
        {
            // A DRAG THAT MOVED NOTHING IS NOT AN UNDO STEP. Pushing here would make Ctrl+Z walk through
            // empty entries — the shape that made the property editor's history unusable before A8.
            Cancel();
            return Common::MakeSuccess( 0U );
        }

        ECS::UIAnimData* const clip = m_Clip;
        UIClipContent          before( std::move( m_Before ) );
        Cancel();

        CommandHistory::Get().PushCommand(
             std::make_unique<UIClipCommand>( clip, std::move( before ), std::move( after ) ) );
        return Common::MakeSuccess( 1U );
    }

    // ── ScopedUIClipEdit ─────────────────────────────────────────────────────────────────────────────

    ScopedUIClipEdit::ScopedUIClipEdit( UIClipEditTransaction& transaction, ECS::UIAnimData* clip )
         : m_Transaction( transaction )
    {
        const auto began = transaction.Begin( clip );
        m_Opened         = began.IsSuccess();
        if ( !m_Opened )
        {
            // Not silent: the edit inside the scope is about to happen anyway, and an author who cannot
            // undo it deserves to know why.
            LOG_ERROR( "[UIClipUndo] this edit will not be undoable: {}", began.GetError() );
        }
    }

    ScopedUIClipEdit::~ScopedUIClipEdit()
    {
        if ( !m_Opened )
        {
            return;
        }
        if ( const auto ended = m_Transaction.End(); !ended.IsSuccess() )
        {
            LOG_ERROR( "[UIClipUndo] {}", ended.GetError() );
        }
    }
} // namespace Desert::Editor
