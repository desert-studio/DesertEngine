#pragma once

#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UILayout.hpp>
#include <Engine/Graphic/Render2D/DrawList2D.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/KeyCodes.hpp>

#include <Engine/UI/UITree.hpp>

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

    // Pointer/mouse state for button interaction, in the SAME pixel space as the canvas viewport (top-left
    // origin). The host (runtime input, editor viewport) supplies it; MouseReleased is the down->up edge so a
    // click fires on release over a button. Pass nullptr to RenderCanvas2D for a non-interactive draw (buttons
    // show their normal state) — e.g. the editor authoring view.
    struct UIInput
    {
        glm::vec2   MousePx       = { 0.0f, 0.0f };
        bool        MouseDown     = false;
        bool        MouseReleased = false;
        // The RIGHT button, held. Only its press edge is used — context menus open on press, as every
        // desktop toolkit does — and that edge is derived inside the frame from UIViewContext::PrevRightDown
        // exactly as the left one is, because a host that reported the edge itself would be a second place
        // computing the same fact.
        bool        MouseRightDown = false;
        float       ScrollDelta   = 0.0f; // mouse-wheel notches this frame (+ = up); drives ScrollView
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

// The `entt::registry&` overloads keep every existing caller's signature; they wrap the registry in an
// EcsUITree and are defined in Engine/UI/Ecs/UICanvasRendererEcs.cpp.
#include <Engine/UI/Ecs/EcsUITree.hpp>

namespace Desert::UI
{
    void BeginUIFrame( UIViewContext& view, entt::registry& reg, const Rect& viewportPx, float frameDtSeconds );
    void EndUIFrame( UIViewContext& view, entt::registry& reg, Graphic::Render2D::DrawList2D& dl,
                     const UIInput* input, entt::entity* focused = nullptr, std::string* outClicked = nullptr,
                     std::vector<std::string>* outMessages = nullptr );
    NO_DISCARD Common::BoolResultStr RenderCanvas2D( UIViewContext& view, entt::registry& reg, entt::entity canvas,
                                                     Graphic::Render2D::DrawList2D& dl,
                                                     const glm::mat4*               worldViewProj = nullptr,
                                                     const UIInput*                 input         = nullptr,
                                                     std::string*                   outClicked    = nullptr,
                                                     entt::entity*                  focused       = nullptr );
} // namespace Desert::UI
