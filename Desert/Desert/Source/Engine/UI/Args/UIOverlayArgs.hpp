#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <string>
#include <glm/glm.hpp>

// Tooltips, context menus, modals and toasts, and what opens them.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // ------------------------------------------------------------------------------------------------
    // OVERLAYS (Ю12) — A TOOLTIP, A CONTEXT MENU, A MODAL AND A TOAST ARE ONE THING FOUR TIMES
    //
    // Each of them is a CANVAS with a higher UICanvasData::SortOrder, drawn after the rest and therefore
    // taking the pointer from whatever it covers — which is exactly what Ю4 built and what
    // `UICanvasContextPair.TheCanvasDrawnLastTakesThePointerFromTheOneBelowIt` already asserts. Nothing
    // here re-implements stacking, hit testing or input capture; this component says WHICH canvas is an
    // overlay, of what kind, and what its policy is.
    //
    // WHO OWNS AN OVERLAY. The author does. An overlay canvas is an ordinary entity of the scene, created
    // by the author (or by the editor's "UI -> Overlay" menu) and destroyed with the scene. NOTHING is
    // created or destroyed at runtime, which is the whole reason this shape was chosen: the alternative —
    // spawning an overlay entity on demand — reproduces the defect class the renderer already paid for
    // once, where a preview kept its slot until somebody remembered to destroy it
    // (Docs/RENDERER_FRAME_STATE.md). Here there is no runtime object to leak, and the overlay survives a
    // scene reload because it IS the scene.
    //
    // WHAT IS RUNTIME, THEN. Only "is it open, where is it, and what does it say", and all of that lives
    // in the (canvas x view) cell (UICanvasContext::Overlay*), never in this component. Two viewports of
    // one scene can therefore have the same context menu open in different places, and opening one in the
    // editor's preview does not dirty the level. That is the same line UIScreen already draws.
    // ------------------------------------------------------------------------------------------------

    // What kind of overlay this canvas is. The kind decides which policy fields below are read, exactly as
    // UICanvasData::RenderMode decides whether WorldScale is read and ScaleMode decides MatchWidthHeight.
    enum class UIOverlayKind
    {
        Tooltip,     // opens after a hover delay on a trigger, follows the pointer, closes when it leaves
        ContextMenu, // opens on a click, closes on Escape / a click outside; a submenu is one of these too
        Modal,       // draws a scrim over the whole view and takes every click that misses its content
        Toast        // a queue of notifications that time out on their own and never take the pointer
    };

    // What makes a trigger fire. Hover is what a tooltip and a submenu use; the two click edges are what a
    // context menu and a modal use.
    enum class UIOverlayTriggerEvent
    {
        Hover,
        LeftClick,
        RightClick
    };

    // Put this on a CANVAS entity. The canvas keeps every field it already had — Sort Order is still what
    // decides which overlay is above which, so a submenu is simply a ContextMenu canvas with a higher one.
    struct UIOverlayData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Overlay;

        PROPERTY( DisplayName( "Kind" ), Category( "UI Overlay" ) )
        UIOverlayKind Kind = UIOverlayKind::Tooltip;

        // How a trigger addresses this overlay. Names are looked up across the scene and a duplicate is
        // REFUSED by name rather than resolved to one of them — the same rule as UI::SoleCanvas, and for
        // the same reason: "there are two and I picked one" is a silent wrong answer.
        PROPERTY( DisplayName( "Name" ), Category( "UI Overlay" ) )
        std::string Name;

        // Tooltip / ContextMenu: the gap in design px between the thing the overlay is placed against (the
        // pointer, or the trigger's rect) and the overlay's own box. Read by UI::PlaceOverlay through
        // UICanvasRenderer2D's overlay update. Meaningless for Modal and Toast, which have no origin to be
        // placed against — their authored anchors place them against the view itself.
        PROPERTY( DisplayName( "Gap" ), Category( "UI Overlay" ) )
        glm::vec2 Gap = glm::vec2( 14.0f, 18.0f );

        // Hover triggers only: how long the pointer must rest on the trigger before this opens. 0 opens on
        // the first frame of contact, which is what a submenu usually wants and what a tooltip never does.
        PROPERTY( DisplayName( "Open Delay" ), Category( "UI Overlay" ), Range( 0.0f, 3.0f ) )
        float OpenDelay = 0.4f;

        // Tooltip: does the box follow the pointer while it stays on the trigger, or is it pinned to the
        // trigger's own rect? Pinned is what a long tooltip on a small button wants; following is what a
        // cursor hint wants.
        PROPERTY( DisplayName( "Follow Pointer" ), Category( "UI Overlay" ) )
        bool FollowPointer = true;

        PROPERTY( DisplayName( "Close On Escape" ), Category( "UI Overlay" ) )
        bool CloseOnEscape = true;

        // A press that the overlay's own content did not take closes it. For a Modal this is the scrim
        // being clicked; for a ContextMenu it is a click anywhere else on screen.
        PROPERTY( DisplayName( "Close On Click Outside" ), Category( "UI Overlay" ) )
        bool CloseOnClickOutside = true;

        // Modal: the dim drawn over the WHOLE view, under this canvas's own content and over everything
        // below it. It is drawn by the walk rather than authored as a panel because it is also what takes
        // the pointer: the scrim is elected as the frame's hot element wherever the modal's own content is
        // not, which is what makes "the click does not reach what is underneath" a consequence of Ю4's
        // single election instead of a second, parallel rule.
        PROPERTY( DisplayName( "Scrim Color" ), Category( "UI Overlay" ), Color )
        glm::vec3 ScrimColor = glm::vec3( 0.0f, 0.0f, 0.0f );

        PROPERTY( DisplayName( "Scrim Opacity" ), Category( "UI Overlay" ), Range( 0.0f, 1.0f ) )
        float ScrimOpacity = 0.55f;

        // Toast: how long one notification stays on screen before it leaves on its own.
        PROPERTY( DisplayName( "Toast Lifetime" ), Category( "UI Overlay" ), Range( 0.5f, 30.0f ) )
        float ToastLifetime = 3.0f;

        // Toast: how many notifications are on screen at once, and therefore how many slots the author
        // wired into this canvas. The runtime publishes exactly this many key sets (overlay.toast.N.text /
        // .visible) into the canvas's per-view locals; anything raised beyond them waits in a bounded
        // queue. See UIOverlay.hpp for why the queue is bounded and what it drops.
        PROPERTY( DisplayName( "Toast Slots" ), Category( "UI Overlay" ), Range( 1, 8 ) )
        int ToastSlots = 3;
    };

    // Put this on an ELEMENT. It says "when the pointer does X to me, open that overlay" — and, for a
    // Toast, "raise a notification saying this".
    //
    // A trigger is not a control: it sits beside a button, a panel or an icon and adds one edge to it. A
    // menu item that opens a submenu is a button with a Hover trigger naming the submenu's canvas, which
    // is why nesting needed no machinery of its own.
    struct UIOverlayTriggerData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::OverlayTrigger;

        PROPERTY( DisplayName( "Overlay" ), Category( "UI Overlay Trigger" ) )
        std::string Overlay; // the UIOverlayData::Name to open

        PROPERTY( DisplayName( "On" ), Category( "UI Overlay Trigger" ) )
        UIOverlayTriggerEvent On = UIOverlayTriggerEvent::Hover;

        // Published into the opened overlay's per-view locals under the key `overlay.text`, so a Text
        // element inside the overlay carrying a UIBinding to that key shows it. For a Toast this is the
        // notification's own line. Empty publishes nothing, and an overlay whose chrome is entirely
        // authored needs none.
        PROPERTY( DisplayName( "Text" ), Category( "UI Overlay Trigger" ) )
        std::string Text;
    };
} // namespace Desert::UI
