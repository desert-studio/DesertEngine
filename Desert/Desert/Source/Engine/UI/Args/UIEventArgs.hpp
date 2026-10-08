#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>
#include <Engine/UI/Args/UILayoutArgs.hpp>

#include <string>
#include <glm/glm.hpp>

// Routed pointer listeners and drag-and-drop.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // WHERE ALONG THE ROUTE a listener fires. A press does not belong to one element: it belongs to the
    // chain from the canvas down to whatever the pointer is over, and every element on that chain is
    // entitled to see it. Until this existed the press was delivered to the elected element ALONE, so
    // "this panel reacts to a click anywhere inside it" had to be spelled as a copy of the component on
    // every leaf, and "this child handled it, the panel behind must not" could not be spelled at all.
    //
    // Bubble is target -> canvas and is what a handler almost always wants: the innermost thing that cares
    // answers first. Tunnel is canvas -> target and is the only order in which an ancestor acts BEFORE its
    // own children, which is what makes Tunnel + StopPropagation express "this panel takes every press
    // inside it and its children never see one". (DOM calls Tunnel the capture phase; Slate calls it
    // tunnelling and routes its Preview* handlers that way. Same thing.)
    enum class UIEventPhase
    {
        Bubble, // fires on the way up: target first, canvas last
        Tunnel  // fires on the way down: canvas first, target last -- before any Bubble listener
    };

    // Pointer callbacks on any UI element. Each message is dispatched exactly like a button's action, so a
    // host that already handles UIButton actions handles these for free. Empty = that edge fires nothing.
    //
    // PRESS AND RELEASE ARE ROUTED along the ancestor chain (see UIEventPhase); ENTER AND EXIT ARE NOT, and
    // that asymmetry is deliberate rather than an omission. Enter/Exit fire on the DIFFERENCE between the
    // chain the pointer was on and the one it is on now, because the naive alternative -- bubble them like
    // a press -- makes a parent that lights up on hover flicker every time the pointer crosses between two
    // of its own children: the shared parent would receive Exit and then Enter although the pointer never
    // left it. DOM draws the same line (mouseenter/mouseleave do not bubble, mouseover/mouseout do) and
    // Slate walks the difference of the two widget paths for the same reason.
    //
    // A listener only fires on an element whose own UIHitTest is All. ChildrenOnly means the pointer does
    // not see THIS element, so it must not be told about a press it is transparent to; Blocking means the
    // element stops the pointer and responds to nothing. Both are the hit-test axis being obeyed by the
    // routing rather than restated in it.
    struct UIPointerEventsData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::PointerEvents;

        PROPERTY( DisplayName( "On Enter" ), Category( "UI Pointer Events" ) )
        std::string OnEnterMessage;

        PROPERTY( DisplayName( "On Exit" ), Category( "UI Pointer Events" ) )
        std::string OnExitMessage;

        PROPERTY( DisplayName( "On Press" ), Category( "UI Pointer Events" ) )
        std::string OnDownMessage;

        PROPERTY( DisplayName( "On Release" ), Category( "UI Pointer Events" ) )
        std::string OnUpMessage;

        // Which pass of the press/release route this listener answers on. No effect on Enter/Exit, which
        // are not routed -- see the note above the struct.
        PROPERTY( DisplayName( "Phase" ), Category( "UI Pointer Events" ) )
        UIEventPhase Phase = UIEventPhase::Bubble;

        // End the press/release route here: no further element on the chain hears this event, in either
        // pass. Independent of whether this listener's own message is empty, so a full-screen scrim can
        // swallow every press inside a modal without emitting anything -- and a scrim that DOES emit is
        // click-outside-to-close, which UIHitTest::Blocking cannot be, because Blocking responds to nothing.
        PROPERTY( DisplayName( "Stop Propagation" ), Category( "UI Pointer Events" ) )
        bool StopPropagation = false;
    };

    // Makes an element draggable. Pressing and moving past a small threshold starts a drag carrying
    // `Payload`; a ghost of the element follows the cursor until release (see UIDropTargetData).
    struct UIDraggableData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Draggable;

        PROPERTY( DisplayName( "Payload" ), Category( "UI Drag" ) )
        std::string Payload; // e.g. "item:sword" — a drop target filters on its prefix

        PROPERTY( DisplayName( "Ghost Opacity" ), Category( "UI Drag" ), Range( 0.0f, 1.0f ) )
        float GhostOpacity = 0.55f;
    };

    // Receives a dropped payload. While a drag is in flight every target that ACCEPTS it outlines itself,
    // so the valid destinations are obvious; releasing over one dispatches "OnDropMessage|payload".
    struct UIDropTargetData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::DropTarget;

        PROPERTY( DisplayName( "Accepts (prefix)" ), Category( "UI Drop" ) )
        std::string Accepts; // "" = anything; "item:" = only payloads starting with it

        PROPERTY( DisplayName( "On Drop" ), Category( "UI Drop" ) )
        std::string OnDropMessage;

        PROPERTY( DisplayName( "Highlight" ), Category( "UI Drop" ), Color )
        glm::vec3 HighlightColor = glm::vec3( 0.35f, 0.75f, 1.0f );
    };
} // namespace Desert::UI
