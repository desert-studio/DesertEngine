#pragma once

// DELIBERATELY NOT SubjectOpenRequest.hpp, for the reason SubjectEditorRegistry.hpp gives at its own top:
// that header opens `namespace Desert::Editor::Core`, and this one is included by EditorLayer.hpp ahead of
// the render-system headers, which spell Desert::Core::Scene as an unqualified `Core::Scene` from inside
// Desert::Editor. Make Desert::Editor::Core visible before them and every one of those names silently
// rebinds to the wrong namespace.
#include <Editor/Panels/IPanel.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Editor
{
    // ── WHO OWNS AN OPEN DOCUMENT ──────────────────────────────────────────────────────────────────────
    //
    // ONE INSTANCE PER SUBJECT, AND IT LIVES HERE. Not in the view that happens to draw it.
    //
    // This container was carved out of Editor/Core/DocumentWell.hpp, which used to be both the owner and
    // the index the well draws — a fusion that was harmless while the well was the only thing that could
    // show a document, and stopped being harmless the moment a SECOND view wanted one. The Clouds window
    // embeds the editor for whichever stage of the sky is selected: the material, the layout, a cloud type.
    // Had it built its own documents, a `.demat` would have had TWO WORKING COPIES — the exact defect
    // Desert/Tests/Editor/MaterialEditStates exists to prevent, one Apply away from a mesh in the level
    // being drawn with the preview's material.
    //
    // The two arrangements that were rejected, and why the record is here rather than in a commit message:
    //
    //   * "The window asks the well to draw it." That is not a layer seam, it is a dependency of one view
    //     on ANOTHER VIEW. Two views, one of which knows about the other — and the third (Details already
    //     asks) would have to know about both.
    //   * "The window keeps its own documents." Two working copies, as above.
    //
    // So ownership is here, and DocumentWell and the Clouds window are two EQUAL views over it. Neither
    // knows the other exists. A view asks this container for the document that edits a subject; what it
    // gets back is the same object the other view has, because there is only one and this is the only
    // thing that can make one.
    //
    // WHAT THIS CLASS DOES NOT DO. It does not destroy anything on its own. Release() hands the document
    // back to the caller, because destruction has to happen behind a device-idle wait and between frames —
    // the ordering ~PreviewViewport and CloseSceneView both established. This knows WHICH document is
    // going; the editor knows WHEN it is safe to let go.
    //
    // Nothing here touches ImGui, the renderer or a global, so the whole thing is drivable by a test with
    // a stub document — which is the only way a rule in this editor gets asserted at all (EditorLayer.cpp
    // is compiled by no suite, scripts/CI/UnreachedSources.sh).

    // What happened to a request to open one.
    //
    // "ALREADY OPEN" IS NOT "OPENED", and the distinction is the whole point of returning an enum instead
    // of a reference. A caller that read the two as one would report a window it did not create, and — far
    // worse — a caller that appended blindly would have created the second working copy this container
    // exists to make impossible. Contract §1.4: an answer that cannot distinguish two facts is a silent
    // wrong answer for one of them.
    enum class DocumentOpenOutcome
    {
        Opened,      // this document is now the one instance for its subject
        AlreadyOpen, // a document for that subject was already here; the one passed in was DESTROYED
        Refused,     // nothing was passed in, or its subject names nothing (SubjectId::IsNull)
    };

    struct DocumentOpenResult
    {
        // The one instance for the subject afterwards. Null only for Refused.
        ISubjectDocument*   Document = nullptr;
        DocumentOpenOutcome Outcome  = DocumentOpenOutcome::Refused;

        [[nodiscard]] bool IsOpen() const noexcept
        {
            return Document != nullptr;
        }
    };

    class OpenDocuments
    {
    public:
        [[nodiscard]] std::size_t Count() const noexcept
        {
            return m_Documents.size();
        }

        [[nodiscard]] bool Empty() const noexcept
        {
            return m_Documents.empty();
        }

        [[nodiscard]] const std::vector<std::unique_ptr<ISubjectDocument>>& Documents() const noexcept
        {
            return m_Documents;
        }

        [[nodiscard]] auto begin() noexcept
        {
            return m_Documents.begin();
        }
        [[nodiscard]] auto end() noexcept
        {
            return m_Documents.end();
        }
        [[nodiscard]] auto begin() const noexcept
        {
            return m_Documents.begin();
        }
        [[nodiscard]] auto end() const noexcept
        {
            return m_Documents.end();
        }

        // THE ONE INSTANCE FOR @p subject, or nullptr. Every view reaches a document through this and
        // through nothing else, which is what makes "the well and the Clouds window show the same object"
        // true by construction rather than by two views agreeing to behave.
        //
        // The null handle is "no asset" and never a document.
        [[nodiscard]] ISubjectDocument* Find( const SubjectId& subject ) const
        {
            if ( subject.IsNull() )
                return nullptr;

            for ( const auto& document : m_Documents )
                if ( document->Subject() == subject )
                    return document.get();
            return nullptr;
        }

        // THE ONLY WAY A DOCUMENT ENTERS THIS EDITOR, and it REFUSES A SECOND ONE FOR A SUBJECT.
        //
        // The refusal is the container's job and not the caller's. Open-or-focus was already written at
        // the one call site that existed, and "already written at the one call site" is precisely the
        // guarantee that stops being true when a second call site appears — which is what the Clouds
        // window is. A blind append would have put two documents over one asset in the same vector, and
        // every consumer downstream (the tab strip, the Ctrl+Tab ring, the slot census, the control
        // channel's `documents`) would have listed the subject twice while the two edited one file.
        //
        // The rejected duplicate is DESTROYED here rather than handed back: it was built to be opened,
        // nothing else can use it, and returning it would make every caller responsible for a lifetime
        // it did not ask for.
        DocumentOpenResult Open( std::unique_ptr<ISubjectDocument> document )
        {
            if ( !document || document->Subject().IsNull() )
                return DocumentOpenResult{ nullptr, DocumentOpenOutcome::Refused };

            if ( ISubjectDocument* already = Find( document->Subject() ) )
                return DocumentOpenResult{ already, DocumentOpenOutcome::AlreadyOpen };

            ISubjectDocument& ref = *document;
            m_Documents.emplace_back( std::move( document ) );
            return DocumentOpenResult{ &ref, DocumentOpenOutcome::Opened };
        }

        // HANDS THE DOCUMENT BACK rather than destroying it: see the note above. Returns nullptr for a
        // subject that is not open — a second close of one window in one frame, which is not an error.
        [[nodiscard]] std::unique_ptr<ISubjectDocument> Release( const SubjectId& subject )
        {
            const auto it = std::find_if( m_Documents.begin(), m_Documents.end(),
                                          [&subject]( const std::unique_ptr<ISubjectDocument>& document )
                                          { return document->Subject() == subject; } );
            if ( it == m_Documents.end() )
                return nullptr;

            std::unique_ptr<ISubjectDocument> released = std::move( *it );
            m_Documents.erase( it );
            return released;
        }

        // Every open document, in one call, for "Close All". Same contract as Release: the caller destroys
        // them, once, behind one device-idle wait rather than one per window. Front to back, so a caller
        // that records them keeps the order they were opened in.
        [[nodiscard]] std::vector<std::unique_ptr<ISubjectDocument>> ReleaseAll()
        {
            std::vector<std::unique_ptr<ISubjectDocument>> released;
            released.reserve( m_Documents.size() );
            for ( auto& document : m_Documents )
                released.emplace_back( std::move( document ) );
            m_Documents.clear();
            m_FramesUndrawn.clear();
            m_DrawnThisFrame.clear();
            return released;
        }

        // ── WAS ANYBODY LOOKING AT IT? ─────────────────────────────────────────────────────────────────
        //
        // A document gives its renderer slot back after a run of frames in which it was not DRAWN — four
        // documents docked as tabs in one node show one tab, and the other three were rendering previews
        // nobody could see while holding three of the six slots (ISubjectDocument::ReleaseView).
        //
        // THE COUNT BELONGS HERE BECAUSE "NOBODY" IS ABOUT ALL THE VIEWS AT ONCE. It used to live in
        // EditorLayer as a map written by the document well's draw loop, which was the whole truth while
        // the well was the only thing that could draw a document. With the Clouds window it is not: a
        // material shown ONLY inside that window would have been counted hidden by the well and had its
        // preview renderer taken away underneath a pane the artist was looking at. One view's answer is
        // not the question, so the question is asked of the owner and every view reports into it.
        //
        // Reset rather than decremented, for the reason the editor's own note gave: the threshold is about
        // a window the user has LEFT, and one visible frame means they have not.

        /// A view drew this document's CONTENTS this frame. Not "the window exists" — a collapsed window
        /// and a dock tab that is not the active one both draw nothing, and those are exactly the cases
        /// the slot release exists for.
        ///
        /// CONST, and the mutable set behind it is not a loophole. `const OpenDocuments&` already means
        /// "you may not change WHICH documents exist" rather than deep const — Find() hands back a
        /// non-const document through a const container for exactly that reason — and a VIEW is given the
        /// const reference precisely so it cannot Open or Release. Reporting that it drew is neither.
        void NoteDrawn( const SubjectId& subject ) const
        {
            if ( !subject.IsNull() )
                m_DrawnThisFrame.insert( subject );
        }

        /// Close the frame's accounting: every open document that no view drew has its run of undrawn
        /// frames extended; every one that was drawn goes back to zero. Called ONCE per frame, after every
        /// view has had its turn — before that the answer would depend on the order the views run in.
        void EndFrame()
        {
            for ( const auto& document : m_Documents )
            {
                const SubjectId& subject = document->Subject();
                if ( m_DrawnThisFrame.count( subject ) != 0 )
                    m_FramesUndrawn.erase( subject );
                else
                    ++m_FramesUndrawn[subject];
            }
            m_DrawnThisFrame.clear();
        }

        /// Consecutive frames in which NO view drew this document. Zero for one that is on screen, and
        /// zero for one nothing knows about — which is the honest answer, because a document that has
        /// never been accounted for has no run of hidden frames behind it.
        [[nodiscard]] uint32_t FramesUndrawn( const SubjectId& subject ) const
        {
            const auto it = m_FramesUndrawn.find( subject );
            return it == m_FramesUndrawn.end() ? 0u : it->second;
        }

        /// Forget one subject's accounting — used when its document is closed, so that reopening it does
        /// not inherit the run of hidden frames the previous window ended on.
        void ForgetDrawHistory( const SubjectId& subject )
        {
            m_FramesUndrawn.erase( subject );
            m_DrawnThisFrame.erase( subject );
        }

    private:
        std::vector<std::unique_ptr<ISubjectDocument>> m_Documents;

        mutable std::unordered_set<SubjectId>   m_DrawnThisFrame;
        std::unordered_map<SubjectId, uint32_t> m_FramesUndrawn;
    };

    // ── THE RELATION TWO VIEWS HAVE TO SATISFY ─────────────────────────────────────────────────────────
    //
    // "Every view that shows the document for a subject shows the SAME OBJECT."
    //
    // Stated as a count of the exceptions rather than as a bool, so a failing suite says how many and
    // which. A view is anything that can answer "what are you showing for this subject?" — in the editor
    // that is DocumentWell and the Clouds window, and neither owns what it shows.
    //
    // WHY THIS IS ASSERTED AND NOT ARGUED. The argument is that OpenDocuments::Open refuses a duplicate
    // and OpenDocuments::Find is the only reader, so it holds by construction. "Holds by construction" is
    // exactly the claim that stops being true the day somebody adds a second way in, and this editor has
    // already lived through that once: before the well existed, documents were built in three hand-wired
    // places, each a route nothing knew about.
    struct DocumentIdentityCensus
    {
        std::size_t Subjects = 0;
        // Subjects for which two views answered with two DIFFERENT objects. Zero, or a `.demat` has two
        // working copies and an Apply from one of them is about to reach the level.
        std::size_t Disagreements = 0;
        // Subjects one view could show and the other could not find at all. Also a failure, and a
        // different one: it means a view is reading a container that is not the owner.
        std::size_t Unreachable = 0;

        [[nodiscard]] bool EveryViewShowsOneObject() const noexcept
        {
            return Disagreements == 0 && Unreachable == 0;
        }
    };

    // @p viewA and @p viewB are callables taking a SubjectId and answering with the ISubjectDocument* that
    // view would draw. Templated so a suite can drive it with two lambdas and no editor anywhere near.
    template <typename ViewA, typename ViewB>
    [[nodiscard]] DocumentIdentityCensus CensusOfDocumentIdentity( const OpenDocuments& documents,
                                                                   const ViewA& viewA, const ViewB& viewB )
    {
        DocumentIdentityCensus census;
        for ( const auto& document : documents )
        {
            if ( !document )
                continue;
            ++census.Subjects;

            const ISubjectDocument* a = viewA( document->Subject() );
            const ISubjectDocument* b = viewB( document->Subject() );
            if ( a == nullptr || b == nullptr )
                ++census.Unreachable;
            else if ( a != b )
                ++census.Disagreements;
        }
        return census;
    }
} // namespace Desert::Editor
