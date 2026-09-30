#include "AnimGraphEdit.hpp"

#include <Common/Core/Logger.hpp>

#include <cstdio>
#include <exception>
#include <format>
#include <utility>

namespace Desert::Editor
{
    namespace G = Animation::Graph;

    bool SameStoredGraph( const G::AnimGraph& a, const G::AnimGraph& b )
    {
        G::AnimGraph left  = a;
        G::AnimGraph right = b;
        // No edit touches the header, and `Serialize` mints one for a graph that has none — per call, so two
        // header-less copies of one graph would never compare equal. Both sides carry the same one.
        auto header = a.Header ? a.Header : b.Header;
        if ( !header )
        {
            const auto minted = G::Deserialize( G::Serialize( a ) );
            if ( minted.IsSuccess() )
                header = minted.GetValue().Header;
        }
        left.Header  = header;
        right.Header = header;
        return G::Serialize( left ) == G::Serialize( right );
    }

    // ── The entry ────────────────────────────────────────────────────────────────────────────────────────

    AnimGraphEditCommand::AnimGraphEditCommand( AnimGraphOwner owner, G::AnimGraph before, G::AnimGraph after )
         : m_Owner( std::move( owner ) ), m_Before( std::move( before ) ), m_After( std::move( after ) )
    {
    }

    bool AnimGraphEditCommand::Apply( const G::AnimGraph& value ) const
    {
        G::AnimGraph* graph = m_Owner.Resolve ? m_Owner.Resolve() : nullptr;
        if ( graph == nullptr )
            return false; // the owner is gone: the history discards the entry
        *graph = value;   // into the SAME object every entity on the graph shares
        if ( m_Owner.AfterRestore )
            m_Owner.AfterRestore();
        return true;
    }

    bool AnimGraphEditCommand::Undo()
    {
        return Apply( m_Before );
    }

    bool AnimGraphEditCommand::Redo()
    {
        return Apply( m_After );
    }

    std::string AnimGraphEditCommand::GetLabel() const
    {
        return std::format( "Edit Anim Graph '{}'", m_Owner.Name );
    }

    // ── The transaction ──────────────────────────────────────────────────────────────────────────────────

    Common::BoolResultStr AnimGraphEditTransaction::Begin( const AnimGraphOwner& owner )
    {
        if ( m_Open )
            return Common::MakeFormattedError<bool>(
                 "an anim graph edit on '{}' is already open; transactions do not nest", m_Owner.Name );
        G::AnimGraph* graph = owner.Resolve ? owner.Resolve() : nullptr;
        if ( graph == nullptr )
            return Common::MakeFormattedError<bool>( "anim graph '{}' does not resolve", owner.Name );
        m_Owner         = owner;
        m_Before        = *graph;
        m_HistoryAtOpen = CommandHistory::Get().Revision();
        m_Open          = true;
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<uint32_t> AnimGraphEditTransaction::End()
    {
        if ( !m_Open )
            return Common::MakeFormattedError<uint32_t>( "no anim graph transaction is open" );
        m_Open                             = false;
        std::optional<G::AnimGraph> before = std::move( m_Before );
        m_Before.reset();
        G::AnimGraph* graph = m_Owner.Resolve ? m_Owner.Resolve() : nullptr;
        if ( graph == nullptr || !before )
            return Common::MakeSuccess( 0u );
        if ( SameStoredGraph( *before, *graph ) )
            return Common::MakeSuccess( 0u );
        CommandHistory::Get().PushCommand(
             std::make_unique<AnimGraphEditCommand>( m_Owner, std::move( *before ), *graph ) );
        // The graph as it stands is the settled state now, and the push is this window's own history move.
        m_Baseline      = *graph;
        m_BaselineOwner = m_Owner.Asset;
        m_HistorySeen   = CommandHistory::Get().Revision();
        return Common::MakeSuccess( 1u );
    }

    void AnimGraphEditTransaction::Cancel()
    {
        m_Open = false;
        m_Before.reset();
    }

    void AnimGraphEditTransaction::Rebase( const G::AnimGraph& graph, uint32_t revision )
    {
        m_Baseline         = graph;
        m_BaselineOwner    = m_Owner.Asset;
        m_BaselineRevision = revision;
        m_HistorySeen      = CommandHistory::Get().Revision();
    }

    uint32_t AnimGraphEditTransaction::Observe( const AnimGraphOwner& owner, uint32_t revision, bool held )
    {
        G::AnimGraph* graph = owner.Resolve ? owner.Resolve() : nullptr;
        if ( graph == nullptr )
        {
            Cancel();
            m_Baseline.reset();
            return 0;
        }
        const uint64_t history = CommandHistory::Get().Revision();

        if ( m_Open )
        {
            // The history rewrote the graph under the gesture (Ctrl+Z while the mouse is down): the before
            // this transaction holds no longer describes the graph, so nothing it would push is true.
            if ( history != m_HistoryAtOpen )
            {
                Cancel();
                m_Owner = owner;
                Rebase( *graph, revision );
                return 0;
            }
            if ( held )
                return 0;
            const auto closed = End();
            m_Owner           = owner;
            Rebase( *graph, revision );
            return closed.IsSuccess() ? closed.GetValue() : 0;
        }

        m_Owner = owner;
        if ( !m_Baseline || m_BaselineOwner != owner.Asset || history != m_HistorySeen )
        {
            // First sight of this owner, or the history moved the graph itself: not an edit of this window's.
            Rebase( *graph, revision );
            return 0;
        }
        if ( held )
        {
            // THE RISING EDGE OPENS ON THE BASELINE: a widget may already have written this frame.
            m_Before        = *m_Baseline;
            m_HistoryAtOpen = history;
            m_Open          = true;
            return 0;
        }
        if ( revision == m_BaselineRevision )
            return 0;
        uint32_t pushed = 0;
        if ( !SameStoredGraph( *m_Baseline, *graph ) )
        {
            CommandHistory::Get().PushCommand(
                 std::make_unique<AnimGraphEditCommand>( owner, *m_Baseline, *graph ) );
            pushed = 1;
        }
        Rebase( *graph, revision );
        return pushed;
    }

    AnimGraphEditTransaction::Scope::Scope( AnimGraphEditTransaction& transaction, const AnimGraphOwner& owner )
         : m_Transaction( transaction )
    {
        m_Began = !transaction.Open() && transaction.Begin( owner ).IsSuccess();
    }

    AnimGraphEditTransaction::Scope::~Scope()
    {
        if ( !m_Began )
            return;
        // A destructor may not throw: the edit inside the scope already happened, so a failed End loses only
        // its undo entry, and the author is told so (UE: FScopedTransaction's EndTransaction).
        try
        {
            try
            {
                if ( const auto ended = m_Transaction.End(); !ended.IsSuccess() )
                    LOG_ERROR( "[AnimGraphUndo] this edit will not be undoable: {}", ended.GetError() );
            }
            catch ( const std::exception& error )
            {
                LOG_ERROR( "[AnimGraphUndo] this edit will not be undoable: {}", error.what() );
            }
        }
        catch ( ... )
        {
            // The logger itself threw: stderr is the channel left to say the entry is gone.
            (void)std::fputs(
                 "[AnimGraphUndo] an edit's undo entry was lost and the logger failed to report why\n", stderr );
        }
    }
} // namespace Desert::Editor
