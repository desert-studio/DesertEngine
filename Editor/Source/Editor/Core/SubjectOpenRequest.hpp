#pragma once

#include <Editor/Core/EditorSubject.hpp>

#include <vector>

namespace Desert::Editor::Core
{
    // The pending "open THIS subject in whatever edits it" requests — the wire between a panel that has a
    // subject (the browser's double-click, the Details panel's button beside a component) and EditorLayer,
    // which is the only place that may create or destroy a document window.
    //
    // WHY THIS IS NOT [[PanelRequests]]. PanelRequests carries a panel NAME and no payload, so it can say
    // "show the Material Preview" and never "show MP_GreenTint". That is exactly why the editor grew three
    // hand-wired file-static inboxes instead (NodeGraphPanel::RequestOpen, MaterialPreviewPanel::
    // RequestPreview, SceneOpenRequest) — each one a private wire for one asset kind. This is the one wire
    // that carries the subject, so the next kind adds a factory rather than a fourth inbox.
    //
    // ONE FIELD NOW, WHERE THERE WERE TWO. The request used to be `{ AssetHandle Subject; AssetTypeID Type; }`,
    // which is a subject with the domain left out because there was only one — and that missing third of the
    // identity is the whole reason a component could not be opened. SubjectId carries it (see
    // Editor/Core/EditorSubject.hpp), so the Details panel's button and the browser's double-click now send
    // the same message.
    class SubjectOpenRequests
    {
    public:
        static void Request( const SubjectId& subject )
        {
            auto& pending = Pending();
            for ( const auto& queued : pending )
            {
                // Requests for the same subject in one frame collapse to one, because open-or-focus would
                // make the second a no-op anyway and two log lines about it would only read like a defect.
                if ( queued == subject )
                    return;
            }
            pending.push_back( subject );
        }

        // Is an open still waiting to be serviced?
        //
        // Asked by the control channel's quiescence census: opening a document is QUEUED here and built
        // between frames, so a reply sent before this went empty would be a reply about a frame drawn
        // before the window existed. Asked of the queue itself rather than of a copy anyone keeps beside
        // it — a copy is a second answer, and the two disagree on exactly the frame that matters.
        [[nodiscard]] static bool HasPending()
        {
            return !Pending().empty();
        }

        // Everything queued since the last drain; clears the queue.
        static std::vector<SubjectId> Drain()
        {
            std::vector<SubjectId> drained;
            drained.swap( Pending() );
            return drained;
        }

    private:
        static std::vector<SubjectId>& Pending();
    };
} // namespace Desert::Editor::Core
