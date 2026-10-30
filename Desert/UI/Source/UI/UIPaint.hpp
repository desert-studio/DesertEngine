#pragma once

#include <UI/UILayout.hpp>
#include <Render2DCore/DrawList2D.hpp>

#include <glm/glm.hpp>

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
    };
} // namespace Desert::UI
