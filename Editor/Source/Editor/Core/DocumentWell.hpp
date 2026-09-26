#pragma once

// DELIBERATELY NOT SubjectOpenRequest.hpp, for the reason SubjectEditorRegistry.hpp gives at its own top: that
// header opens `namespace Desert::Editor::Core`, and this one is included by EditorLayer.hpp ahead of the
// render-system headers, which spell Desert::Core::Scene as an unqualified `Core::Scene` from inside
// Desert::Editor. Make Desert::Editor::Core visible before them and every one of those names silently
// rebinds to the wrong namespace.
#include <Editor/Core/OpenDocuments.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    // ONE VIEW OF THE OPEN DOCUMENTS — the tabbed well and the index beside it. NOT their owner.
    //
    // THE OWNERSHIP MOVED OUT OF HERE, and the move is the point rather than tidying. This class used to
    // hold the `vector<unique_ptr<ISubjectDocument>>` itself, which was indistinguishable from being the
    // model while it was the only view. It is not any more: the Clouds window (Editor/Panels/Clouds) shows
    // the editor for whichever stage of the sky is selected, and a second view that built its OWN documents
    // would have given a `.demat` two working copies — the defect Desert/Tests/Editor/MaterialEditStates
    // exists to prevent. Editor/Core/OpenDocuments.hpp now owns them and the two views are equal over it;
    // this one does not know the other exists.
    //
    // WHAT IS LEFT HERE IS VIEW STATE, and it is view state precisely because the OTHER view does not want
    // it: the Ctrl+Tab ring is about the tab strip, and the recently-closed list is what THIS window offers
    // in place of being blank. The Clouds window has a rail of six stages instead and neither would mean
    // anything there.
    //
    // The other half of the split PanelRegistry describes: tools live there, documents live in
    // OpenDocuments, and nothing lives in both. A tool is a setting the user keeps; a document is a window
    // over one subject that exists only while that subject is being edited, and closing it is a DESTRUCTION
    // — that is what returns its Scene, its SceneRenderer and one of the six renderer slots.
    //
    // Because the two are separate owners, the View menu, the command palette and `--open-panel` cannot
    // list a document: they are loops over the registry, and there are no documents in it. That is the
    // point. The alternative on offer was a predicate on each of those loops, which would have left the same
    // trap in three places at once and made every later loop responsible for knowing about it.
    //
    // Nothing here touches ImGui, the renderer or a global, so the whole thing is drivable by a test with a
    // stub document — which is the only way a rule in this editor gets asserted at all (EditorLayer.cpp is
    // compiled by no suite, scripts/CI/UnreachedSources.sh).

    // The visible half of a document's name. A document's panel name is "<display>###doc<subject>"
    // (DocumentTitle), and every place that shows one to a person wants the part before the "###":
    // a user closes a window titled "M_Crate_Painted", not one titled
    // "M_Crate_Painted###docasset:2:3333333333333333333". One function rather than the three separate
    // find-and-erase copies this rule used to have.
    [[nodiscard]] inline std::string DocumentDisplayName( const std::string& panelName )
    {
        std::string label = panelName;
        if ( const auto pos = label.find( "###" ); pos != std::string::npos )
            label.erase( pos );
        return label;
    }

    // A document that WAS open. Kept by value, because the panel it came from is gone by the time anyone
    // reads this: a pointer here would be the one dangling reference the whole split exists to avoid.
    struct ClosedDocument
    {
        std::string         DisplayName;
        SubjectId           Subject;
    };

    class DocumentWell
    {
    public:
        // How many closed documents the empty state offers back. Long enough to undo a mistaken close and
        // short enough that the list is read rather than scanned.
        static constexpr std::size_t kRecentlyClosedLimit = 6;

        // A VIEW IS CONSTRUCTED OVER ITS MODEL, and the reference is not optional: a well with no documents
        // to show is a different object from a well that has lost track of where they are. Held by pointer
        // rather than by reference only so the class stays assignable, which the editor's member ordering
        // wants.
        explicit DocumentWell( const OpenDocuments& documents ) noexcept : m_Documents( &documents )
        {
        }

        // The document this view would draw for @p subject: the ONE instance, asked of its owner. Every
        // view answers this question the same way, and CensusOfDocumentIdentity (OpenDocuments.hpp)
        // asserts that they agree.
        [[nodiscard]] ISubjectDocument* Showing( const SubjectId& subject ) const
        {
            return m_Documents->Find( subject );
        }

        // Marks @p subject as the most recently used document. Focusing one is the only thing that reorders
        // the Ctrl+Tab ring; drawing one does not, or the ring would reorder itself every frame and Ctrl+Tab
        // would never leave the front two.
        void Touch( const SubjectId& subject )
        {
            if ( !m_Documents->Find( subject ) )
                return;
            std::erase( m_MostRecent, subject );
            m_MostRecent.insert( m_MostRecent.begin(), subject );
        }

        // A document was opened, or an open one was asked for by name. Touch, plus the one rule that makes a
        // closable well safe: whoever opens a document gets to SEE it, so a well the user closed comes back.
        // Plain Touch (focus following a click on a document tab) leaves the window alone — the user who
        // closed it is looking at that document already.
        void Opened( const SubjectId& subject )
        {
            if ( m_Documents->Find( subject ) == nullptr )
                return;
            Touch( subject );
            m_WindowOpen = true;
        }

        // A document has left the owner. Drops it from the ring and records it under RecentlyClosed.
        //
        // TOLD, NOT DISCOVERED. The well used to perform the release itself, so it learned of a close by
        // doing it; a view cannot, and a view that polled the owner for departures would have to keep a
        // second copy of the open set to notice one — the duplicated-state shape the split removes. The
        // editor performs the release (it is the only thing that knows the device is idle) and tells the
        // views. A view that is not told simply keeps a stale ring entry, which Touch and NextMostRecent
        // both filter through the owner, so it cannot become a dangling reference.
        void NoteClosed( const ISubjectDocument& document )
        {
            std::erase( m_MostRecent, document.Subject() );
            RememberClosed( ClosedDocument{ DocumentDisplayName( document.GetName() ), document.Subject() } );
        }

        // CTRL+TAB. The document after @p current in most-recently-used order, wrapping to the front. This
        // is what makes ten open documents bearable: the tab you want is usually the one you were just in,
        // and past about six the strip has it off-screen.
        //
        // nullopt when there is nothing to switch to (fewer than two open). A @p current that is not open —
        // the focus is on a tool, or on nothing — answers with the most recently used document, which is
        // where "back to what I was editing" should land.
        [[nodiscard]] std::optional<SubjectId> NextMostRecent( const SubjectId& current ) const
        {
            if ( m_MostRecent.empty() )
                return std::nullopt;

            const auto it = std::find( m_MostRecent.begin(), m_MostRecent.end(), current );
            if ( it == m_MostRecent.end() )
                return m_MostRecent.front();

            if ( m_MostRecent.size() < 2 )
                return std::nullopt;

            const auto next = std::next( it );
            return next == m_MostRecent.end() ? m_MostRecent.front() : *next;
        }

        // Most recently used first. The Documents menu and the Ctrl+Tab ring read the same order.
        [[nodiscard]] const std::vector<SubjectId>& MostRecentOrder() const noexcept
        {
            return m_MostRecent;
        }

        // Newest first, capped at kRecentlyClosedLimit. What the empty area offers instead of being blank.
        [[nodiscard]] const std::vector<ClosedDocument>& RecentlyClosed() const noexcept
        {
            return m_RecentlyClosed;
        }

        // ── THE WINDOW ─────────────────────────────────────────────────────────────────────────────────
        // The "Documents" window has a close button like any tool (UE's document area works the same way).
        // Closing the window closes no document: the documents live in OpenDocuments, not here.
        [[nodiscard]] bool IsWindowOpen() const noexcept
        {
            return m_WindowOpen;
        }
        // The flag ImGui's close button writes (Begin's p_open).
        [[nodiscard]] bool& WindowOpenFlag() noexcept
        {
            return m_WindowOpen;
        }
        void ShowWindow() noexcept
        {
            m_WindowOpen = true;
        }
        void CloseWindow() noexcept
        {
            m_WindowOpen = false;
        }

        // THE WINDOW'S LINE IN THE EDITOR LAYOUT (imgui.ini and every View > Layouts file), so open or closed
        // survives a restart and travels with a named layout. A layout that has no line — the default, or
        // one saved before the window could close — means open.
        static constexpr std::string_view kLayoutOpen   = "Open=1";
        static constexpr std::string_view kLayoutClosed = "Open=0";

        [[nodiscard]] std::string_view LayoutLine() const noexcept
        {
            return m_WindowOpen ? kLayoutOpen : kLayoutClosed;
        }
        // False for a line this build does not write; the window state is then left untouched and the
        // caller reports the line.
        [[nodiscard]] bool ReadLayoutLine( std::string_view line ) noexcept
        {
            if ( line == kLayoutOpen )
                m_WindowOpen = true;
            else if ( line == kLayoutClosed )
                m_WindowOpen = false;
            else
                return false;
            return true;
        }

    private:
        void RememberClosed( ClosedDocument closed )
        {
            // Reopening and re-closing one asset must not fill the list with copies of it.
            std::erase_if( m_RecentlyClosed, [&closed]( const ClosedDocument& previous )
                           { return previous.Subject == closed.Subject; } );
            m_RecentlyClosed.insert( m_RecentlyClosed.begin(), std::move( closed ) );
            if ( m_RecentlyClosed.size() > kRecentlyClosedLimit )
                m_RecentlyClosed.resize( kRecentlyClosedLimit );
        }

        const OpenDocuments* m_Documents = nullptr;
        // Subjects, most recent first. Subjects and not pointers: an identity cannot dangle, and the whole
        // point of the split is that a document's lifetime is short.
        std::vector<SubjectId>           m_MostRecent;
        std::vector<ClosedDocument>      m_RecentlyClosed;
        bool                             m_WindowOpen = true;
    };

    // How many of the six renderer slots the OPEN DOCUMENTS are holding right now.
    //
    // Beside PendingRendererSlotDemand (SubjectEditorRegistry.hpp), which answers the other half: that one
    // counts claims that have not landed, this one counts the ones that have. Together they are what the
    // status bar shows and what a refusal has to be able to explain, and both are free functions over a
    // range for the same reason — EditorLayer.cpp is compiled by no suite, so a rule written there is a rule
    // nothing can assert.
    template <typename Range>
    [[nodiscard]] uint32_t ViewsHeldByDocuments( const Range& documents )
    {
        uint32_t held = 0;
        for ( const auto& document : documents )
            if ( document && document->HoldsView() )
                ++held;
        return held;
    }

    // THE PARTITION, COUNTED. The editor draws the tools and then the documents, and the two owners must
    // between them account for every panel exactly once: a panel in neither is one nobody can reach, and a
    // panel in both is the shared-container defect coming back wearing a second name.
    struct PanelCensus
    {
        std::size_t Tools     = 0;
        std::size_t Documents = 0;
        std::size_t Total     = 0;
        // Whether the tool side is free of documents. False is the defect this whole split removes: it is
        // exactly the state in which the View menu can list a document again.
        bool ToolsHoldNoDocument = true;
    };

    template <typename PanelRange, typename DocumentRange>
    [[nodiscard]] PanelCensus CensusOfPanels( const PanelRange& tools, const DocumentRange& documents )
    {
        PanelCensus census;
        for ( const auto& tool : tools )
        {
            ++census.Tools;
            ++census.Total;
            if ( dynamic_cast<const ISubjectDocument*>( &*tool ) )
                census.ToolsHoldNoDocument = false;
        }
        for ( const auto& document : documents )
        {
            (void)document;
            ++census.Documents;
            ++census.Total;
        }
        return census;
    }
} // namespace Desert::Editor
