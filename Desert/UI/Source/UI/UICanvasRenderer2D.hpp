#pragma once

#include <UI/UICanvasContext.hpp>
#include <UI/UILayout.hpp>
#include <Render2DCore/DrawList2D.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/KeyCodes.hpp>

#include <UI/UITree.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Render2D-backed canvas renderer — the replacement for the ImGui-based RenderCanvas. Walks the scene's
// UICanvas / UILayout tree and records geometry into a DrawList2D, which the Render2D GPU backend then
// batches into real draw calls. This slice draws flat-colour panels + button backgrounds (screen-space
// canvas); sprites / 9-slice / text / effects / clipping land in later slices until it reaches parity with
// the ImGui path, which is then removed.
namespace Desert::UI
{
    // Modifier keys held when a key event happened — a bit set, so Shift+Ctrl is both bits.
    enum class UIKeyMods : uint8_t
    {
        None  = 0,
        Shift = 1 << 0,
        Ctrl  = 1 << 1,
        Alt   = 1 << 2,
        Super = 1 << 3,
    };

    constexpr UIKeyMods operator|( UIKeyMods a, UIKeyMods b )
    {
        return static_cast<UIKeyMods>( static_cast<uint8_t>( a ) | static_cast<uint8_t>( b ) );
    }

    constexpr bool HasMod( UIKeyMods set, UIKeyMods m )
    {
        return ( static_cast<uint8_t>( set ) & static_cast<uint8_t>( m ) ) != 0;
    }

    // One key press as the host saw it (UE's FKeyEvent: key, modifier state, repeat). @p Repeat marks an
    // auto-repeat of a key already held.
    struct UIKeyEvent
    {
        Common::KeyCode Key    = Common::KeyCode::Space;
        UIKeyMods       Mods   = UIKeyMods::None;
        bool            Repeat = false;
    };

    // ONE KEYBOARD PER USER (Slate's FSlateUser: one focus path per user, and keys go only down it). A host
    // that draws several views for one person — the editor's level canvases and the widget gallery over
    // them — hands every view the SAME UIKeyboardUser through UIInput::Keyboard, and only the view named
    // Owner sees the frame's keys and typed text; every other view sees none and drops its focus, so one
    // keystroke can never land in two fields. A press that lands on an element of a view, or a capture it
    // takes, makes that view the owner (EndUIFrame) — the click that moves focus in Slate. Tab and the
    // arrows then move focus inside the owner only. Which view owns the keyboard before anything has been
    // clicked is the host's to say (it sets Owner); null owns nothing.
    struct UIKeyboardUser
    {
        const UIViewContext* Owner = nullptr;
    };

    // Pointer/mouse state for button interaction, in the SAME pixel space as the canvas viewport (top-left
    // origin). The host (runtime input, editor viewport) supplies it; MouseReleased is the down->up edge so a
    // click fires on release over a button. Pass nullptr to RenderCanvas2D for a non-interactive draw (buttons
    // show their normal state) — e.g. the editor authoring view.
    struct UIInput
    {
        glm::vec2 MousePx       = { 0.0f, 0.0f };
        bool      MouseDown     = false;
        bool      MouseReleased = false;
        // The pointer is over THIS view. False when a view drawn above it covers the pointer (the editor's
        // widget gallery over the level's canvases — Slate's hit-test grid, where the topmost window wins):
        // nothing here is elected, so nothing hovers, and a press or a wheel notch lands on nothing. A control
        // that already holds the pointer (UIViewContext::Capture) keeps following it, and a release still
        // closes an open popup, as a click on another window closes a menu.
        bool PointerOver = true;
        // The RIGHT button, held. Only its press edge is used — context menus open on press, as every
        // desktop toolkit does — and that edge is derived inside the frame from UIViewContext::PrevRightDown
        // exactly as the left one is, because a host that reported the edge itself would be a second place
        // computing the same fact.
        bool MouseRightDown = false;
        // The MIDDLE button, held — the pan button of every graph, curve and node editor. Like the right one
        // it is reported held and its press edge is derived inside the frame (UIViewContext::PrevMiddleDown);
        // the press lands in UIViewContext::LastPress with Button = Middle.
        bool  MouseMiddleDown = false;
        float ScrollDelta     = 0.0f; // mouse-wheel notches this frame (+ = up); drives ScrollView
        // THE HOST'S DOUBLE-CLICK INTERVAL, in seconds — the OS setting (Windows GetDoubleClickTime, macOS
        // NSEvent.doubleClickInterval), which is the user's and not the UI's to choose. Whether a press IS a
        // double click is decided inside the frame, in one place (UIViewContext::LastPress), from this and the
        // view's own clock, as the press edges are: a host that reported "double-clicked" itself would be a
        // second place counting the same clicks. 0.5 s is the default of both systems.
        float DoubleClickSeconds = 0.5f;
        // The modifiers HELD this frame, for the pointer: a Shift-click extends a list's selection and a
        // Ctrl/Cmd-click toggles a row (UE FPointerEvent::GetModifierKeys). Keys carry their own.
        UIKeyMods Mods = UIKeyMods::None;
        // THE KEYBOARD, AS EVENTS. Every key press the host saw this frame, in the order it saw them, and the
        // UTF-8 text typed. The host reports keys and nothing else: what a key MEANS to the UI (Tab advances
        // focus, Enter activates, Escape closes the innermost overlay, Up/W and Down/S step focus) is decided
        // inside the UI, in one place, rather than once per host as it was when this struct carried a bool
        // per meaning. A host with no keyboard leaves both empty.
        std::vector<UIKeyEvent> Keys;
        std::string             TypedText; // UTF-8 chars typed this frame (drives the focused InputField)
        // The system clipboard, for the focused InputField's copy/cut/paste. The host owns it (it wraps its
        // window); null = no clipboard, and those three keys do nothing.
        IUIClipboard* Clipboard = nullptr;
        // The user this frame's keys belong to, shared by every view the host draws for that user
        // (UIKeyboardUser). Null: this host draws one view, and it holds the keyboard.
        UIKeyboardUser* Keyboard = nullptr;

        // Does @p view hold this user's keyboard (UIKeyboardUser)?
        NO_DISCARD bool HoldsKeyboard( const UIViewContext& view ) const
        {
            return Keyboard == nullptr || Keyboard->Owner == &view;
        }

        // Was @p key pressed (or auto-repeated) at least once this frame?
        NO_DISCARD bool Pressed( Common::KeyCode key ) const
        {
            return std::ranges::any_of( Keys, [key]( const UIKeyEvent& k ) { return k.Key == key; } );
        }
    };

    // --- A FRAME OF A VIEW, AND THE CANVASES INSIDE IT ------------------------------------------------
    //
    // A view draws N canvases per frame, so the things there is exactly ONE of per view per frame cannot
    // live inside the per-canvas walk. Three of them did, and each was a defect the moment a second canvas
    // appeared: the clock (the second walk measured dt ~ 0 and its eases froze — the same defect U3 fixed
    // BETWEEN views, reappearing between the canvases of one), the hot hand-over (the last canvas drawn
    // handed over its own election as the whole answer, so an overlay canvas erased the HUD's hot element
    // by existing) and the pointer routing (enter/exit fired per canvas, so crossing from a HUD button to
    // an overlay button reported neither, or both, depending on draw order).
    //
    // Hence the boundary:
    //
    //     BeginUIFrame( view, reg, viewportPx, frameDtSeconds );
    //     for ( canvas : CanvasesInDrawOrder( reg ) )  RenderCanvas2D( view, reg, canvas, ... );
    //     EndUIFrame( view, reg, dl, input, ... );
    //
    // THE VIEWPORT IS A PARAMETER OF THE FRAME AND NOT OF THE CANVAS. It used to be passed to every
    // RenderCanvas2D call, so a frame could hand two canvases two different rectangles and nothing said
    // so — and the overlay update, which runs after the walks and has to place a box inside the view,
    // would have needed a third copy of the same number. A view is one framebuffer; stating that once is
    // what makes the copies unable to disagree.
    //
    // RenderCanvas2D REFUSES outside that pair rather than drawing a frozen frame — see
    // UICanvasContext::FrameOpen.

    // Open a frame of @p view: advance its clock, drop the cells of canvases that no longer exist, and
    // start a fresh hot election. Rebinds the view to @p reg (dropping everything) when it is looking at
    // another scene, because entity ids are unique only inside a registry. @p viewportPx is where this view
    // draws, for the whole frame — see above. @p frameDtSeconds is the frame's timestep as the HOST steps
    // it (the editor's frame Timestep, fixed under --play): the view's eases, tweens, transitions and UI
    // clips advance by exactly that, so frame N of a fixed-step run is tick N. Clamped to 0.1 s.
    void BeginUIFrame( UIViewContext& view, IUITree& tree, const Rect& viewportPx, float frameDtSeconds );

    // Close it: hand this frame's hot election to the next frame, route the pointer events that election
    // implies (enter/exit, down/up, drop) across EVERY canvas the frame drew, advance keyboard focus on
    // Tab, run the overlay state machine on the election just made (UIOverlay.hpp), and draw the drag ghost
    // on top of all of them. @p outMessages collects every message fired;
    // without it they fall back to @p outClicked while it is still empty.
    void EndUIFrame( UIViewContext& view, IUITree& tree, Graphic::Render2D::DrawList2D& dl, const UIInput* input,
                     NodeId* focused = nullptr, std::string* outClicked = nullptr,
                     std::vector<std::string>* outMessages = nullptr );

    // Emit @p canvas into @p dl in pixel coordinates within the frame's viewport (UIViewContext::ViewportPx,
    // given to BeginUIFrame). When the canvas is
    // WorldSpace, @p worldViewProj (camera projection*view) billboards + distance-scales it to the screen;
    // pass nullptr for screen-space-only hosts. @p input drives button hover/press; a click on release writes
    // the button's encoded action to @p outClicked (see the runtime dispatcher).
    //
    // @p canvas IS ASKED, NEVER GUESSED. This used to take only the registry and elect
    // `*reg.view<UICanvasComponent>().begin()` — entt's iteration order — so a scene's second canvas was
    // never drawn by anything and nothing said so. See UICanvasLayout.hpp for the four ways to obtain one
    // (CanvasOf / CanvasCount / SoleCanvas / CanvasesInDrawOrder); none of them can silently pick a winner.
    //
    // The result separates the three outcomes a bool could not: an ERROR means @p canvas is not a canvas of
    // @p reg (a caller bug, named) or no frame of @p view is open, success(false) means the canvas exists
    // but drew nothing this frame (it is not Visible, or a WorldSpace canvas is behind the camera),
    // success(true) means it was drawn.
    //
    // @p view carries EVERYTHING the walk remembers between frames, and the cell it uses is the one keyed
    // by (@p canvas x @p view) — looked up here rather than passed in, so no caller can hand one canvas's
    // screen stack to another canvas's walk. See UICanvasContext.hpp for why neither coordinate alone was
    // enough.
    NO_DISCARD Common::BoolResultStr
    RenderCanvas2D( UIViewContext& view, IUITree& tree, NodeId canvas, Graphic::Render2D::DrawList2D& dl,
                    const glm::mat4* worldViewProj = nullptr, const UIInput* input = nullptr,
                    std::string* outClicked = nullptr, NodeId* focused = nullptr );
} // namespace Desert::UI
