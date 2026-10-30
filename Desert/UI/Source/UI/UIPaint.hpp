#pragma once

#include <UI/UILayout.hpp>
#include <Render2DCore/DrawList2D.hpp>
#include <Common/Core/KeyCodes.hpp>

#include <glm/glm.hpp>

#include <vector>

// WHAT A PAINT LEAF IS HANDED — Slate's SLeafWidget::OnPaint( Args, AllottedGeometry, …, OutDrawElements ).
//
// A tree may give any node its own drawing (IUITree::Paint). The walk calls it once per frame for every node
// with a rect, after the node's own widgets have drawn and before its children, into THE SAME draw list the
// widgets write: there is one list per frame, so a painted node clips, transforms, layers and retains exactly
// as a panel or a slider does, and the renderer has no per-widget branch for it.
//
// Every value is in the node's own layout space — the space `Box` is in, before the draw list's accumulated
// render transform — so drawing at Box's corners lands on the node's rect, and Pointer can be compared with
// Box directly even when an ancestor is rotated or scaled.
namespace Desert::UI
{
    struct UIInput;
    class IUICanvasResources;
    class ElementStyle;

    struct UIPaintArgs
    {
        Graphic::Render2D::DrawList2D& Dl;
        Rect                           Box{};               // the node's rect (layout px)
        float                          Scale = 1.0f;        // the canvas scale: px per authored unit
        glm::vec2                      Pointer{ 0.0f };     // the pointer in Box's space (HasPointer only)
        bool                           HasPointer  = false; // the host fed input to this frame
        bool                           Interactive = false; // the node may be interacted with (hit-test rules)
        bool                           Hot         = false; // the pointer is over the node (or its children)
        bool                           Pressed     = false; // Hot and the left button is held
        bool                           Captured    = false; // the node holds the pointer (UIViewContext::Capture)
        // The frame's input as THIS view sees it — its keys and typed text only while the view holds its
        // user's keyboard (UIKeyboardUser) — or null for a non-interactive draw. A leaf reads keys from here
        // and never from the host, so it cannot take a keystroke meant for another view.
        const UIInput* Input = nullptr;
        // What a leaf that draws TEXT needs (a code editor, Slate's SMultiLineEditableText under a custom
        // paint): the view's fonts and measuring (null for a draw with no resources), the node's resolved
        // style (its theme's colours and fonts; null = none, every query takes the leaf's local value) and
        // the view's clock (double clicks, caret blink).
        IUICanvasResources* Resources = nullptr;
        const ElementStyle* Style     = nullptr;
        double              Time      = 0.0;
        // The node holds its view's keyboard focus. Only a node that takes text input (IUITree::
        // TakesTextInput) is ever focused through here: the walk focuses it on the press that lands on it.
        bool Focused = false;
        // SLATE'S FReply::Handled for keys: a focused leaf appends every key it acted on (a caret move, a
        // Tab it indented with), so the same key does not also move focus or reach another control. Null
        // for a draw with no input.
        std::vector<Common::KeyCode>* HandledKeys = nullptr;
    };
} // namespace Desert::UI
