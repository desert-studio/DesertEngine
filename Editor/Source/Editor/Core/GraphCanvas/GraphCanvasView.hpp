#pragma once

#include "GraphCanvas.hpp"

#include <optional>
#include <span>
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

    // ── COMMENT BOXES AND FIND… (design §7.2; UE Graph Editor) ───────────────────────────────────────
    //
    // A comment box is an `imgui-node-editor` GROUP node: its title drags every node lying inside it, its
    // edges resize it. All of the calls below run between `ed::Begin` and `ed::End` of the canvas they act
    // on, except `FindPopup::Draw` / `CommentTextPopup`, which run where ImGui popups may (inside
    // `ed::Suspend` / `ed::Resume`).

    /// Draws one comment box. @p x / @p y is its top-left (title included), @p width / @p height the framed
    /// area under the title. A box the canvas has never seen (@p fresh) is pushed in; a known one is read
    /// back. True when the user moved or resized it.
    [[nodiscard]] bool DrawCommentBox( ElementId id, bool fresh, const std::string& text, float& x, float& y,
                                       float& width, float& height );

    /// Where `C` puts a new box: around the selected nodes when there are any (UE), otherwise a box of a
    /// default size at the mouse. X/Y is the box's top-left, Width/Height its framed area.
    [[nodiscard]] CanvasRect NewCommentRect();

    /// `C` / `Ctrl+F` pressed while this canvas has the keyboard and no text field is being typed into.
    [[nodiscard]] bool CommentKeyPressed();
    [[nodiscard]] bool FindKeyPressed();

    /// Selects the node and moves the view onto it — what a Find… result does.
    void FocusNode( ElementId id );

    /// The Find… popup of one canvas: a query field over the canvas's node names; picking one answers its
    /// index in the names passed to `Draw`.
    class FindPopup
    {
    public:
        explicit FindPopup( std::string popupId ) : m_PopupId( std::move( popupId ) )
        {
        }

        /// Opens on the next `Draw`, with the query field focused and the previous query kept.
        void Open();

        /// Draws the popup when open; the index in @p names the user picked, on that frame only.
        [[nodiscard]] std::optional<size_t> Draw( std::span<const std::string> names );

    private:
        std::string m_PopupId;
        std::string m_Query;
        bool        m_OpenRequested = false;
        bool        m_FocusQuery    = false;
    };

    /// The rename popup of a comment box (double-click its title). True on the frame @p text changed.
    [[nodiscard]] bool CommentTextPopup( const char* popupId, std::string& text );

} // namespace Desert::Editor::Graph
