#pragma once

#include "GraphCanvas.hpp"

#include <string>

namespace ax::NodeEditor
{
    struct EditorContext;
}

// ── THE CHROME AND THE VIEW, ONE SPELLING FOR EVERY GRAPH DOCUMENT ───────────────────────────────────
//
// The half of `GraphCanvas` that needs ImGui and `imgui-node-editor`. Split from the identity half so
// that the identity half stays testable without a UI context — `GraphCanvasIdentity` compiles that one
// and not this one.
namespace Desert::Editor::Graph
{
    /// "Show everything" — the `F` key of every node editor ever written, and the thing the anim graph
    /// did not have. Without it a view that went wrong is a document that can never be used again, which
    /// is exactly what happened: see `DeferredFrameAll`.
    void FrameAll( ax::NodeEditor::EditorContext* context );

    /// "Show what is selected" (Shift+F). Falls back to framing everything when nothing is selected,
    /// because a key that silently does nothing is indistinguishable from a broken one.
    void FrameSelection( ax::NodeEditor::EditorContext* context );

    /// Pushes the model's stored position into the canvas for an element the canvas has never seen.
    /// Call BEFORE `ed::BeginNode`. A no-op for an element the canvas already knows — telling it again
    /// every frame is how a node becomes undraggable.
    void PushNodePosition( const PlannedNode& node );

    /// Reads the canvas's idea of where the node is back into the model. Call AFTER `ed::EndNode`.
    /// Returns true when @p x / @p y actually moved, so a caller can tell an edit from a redraw.
    [[nodiscard]] bool PullNodePosition( const PlannedNode& node, float& x, float& y );

    /// The `Frame All` / `Frame Sel` toolbar pair. One spelling, because two documents drawing the same
    /// two buttons with two labels is how a shortcut stops being a shortcut.
    void DrawViewButtons( ax::NodeEditor::EditorContext* context );

    /// The document's one status line. Both graph documents grew this independently, with the same two
    /// colours and the same two meanings.
    void DrawStatusLine( const std::string& status, bool isError );

} // namespace Desert::Editor::Graph
