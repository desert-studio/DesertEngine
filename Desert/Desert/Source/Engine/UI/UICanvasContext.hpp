#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UILayout.hpp>
#include <Engine/UI/UIMaterialSource.hpp>
#include <Engine/UI/UIRenderTextureSource.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The runtime state of the UI walk, keyed by the PAIR (canvas x view).
//
// WHY THIS FILE EXISTS. All of it used to be file-scope variables inside UICanvasRenderer2D.cpp, i.e. one
// set per process, and the engine draws more than one canvas per frame:
//
//   * the editor builds a Render::RenderRegistry — and with it an EditorUIPass — per open scene document,
//     so two viewports walk two scenes into the same variables in one frame;
//   * the UI Editor panel walks the SAME scene a second time into its own offscreen target. That walk is
//     deliberately non-interactive (input = nullptr), but it still ran the hand-over at the end of the walk
//     and so cleared the viewport's elected hot element every frame the panel was open. That one needs no
//     second scene to reproduce;
//   * entt::entity is unique only INSIDE its registry, so the per-entity clocks below (hover, tween) had a
//     key that entity 7 of scene A and entity 7 of scene B both answered to. Same shape as the pipeline
//     cache key that dropped five fields.
//
// U3 fixed the VIEW half of that: a host owns its state, hands it to every walk it makes, and two views
// cannot reach each other's because they never share the object. The CANVAS half was still missing, and it
// is not a smaller one — a view draws EVERY canvas of its scene (UI::CanvasesInDrawOrder), so a HUD canvas and
// a menu canvas walked into one screen machine, one hover-clock table and one seed. The second walk of
// every frame re-seeded `Screen` to a name from its own tree because the first walk's name is foreign to it,
// which flips both canvases back to their first screen forever.
//
// So the key is the pair, exactly as Docs/RENDERER_FRAME_STATE.md keys a renderer's per-frame state by
// (frame x slot) rather than by either alone: UIViewContext is one view's half, UICanvasContext is one
// canvas's half INSIDE it, and neither exists without the other. A cell of that table is reachable only
// through the view that owns it, so a canvas drawn by two views has two independent cells and no code path
// can name one of them from the other.
//
// WHAT DELIBERATELY DID NOT MOVE HERE. Runtime UI state stays OUT of the ECS components (UI_ROADMAP.md
// section F): navigating screens or easing a hover in the editor must not rewrite the authored scene. This
// type is that decision made explicit, not a reversal of it.
namespace Desert::UI
{
    // An in-flight drag. Cross-frame AND cross-canvas: a drag survives from the press that starts it to the
    // release that drops it, and the drop may land on another canvas of the same view (that is what makes a
    // drag from a HUD slot onto an inventory overlay expressible). It therefore belongs to the VIEW.
    struct UIDragState
    {
        bool         Active  = false; // past the threshold: the ghost is up and a drop can land
        bool         Pending = false; // pressed on a draggable, still deciding drag vs click
        entt::entity Source  = entt::null;
        std::string  Payload;
        glm::vec2    Size{ 0.0f };
        glm::vec2    PressPos{ 0.0f };
        float        Ghost = 0.55f;
    };

    // ONE CANVAS AS ONE VIEW SEES IT — the (canvas x view) cell. Lives inside the UIViewContext below and
    // is obtained only from it, so the cell and its two coordinates cannot drift apart.
    struct UICanvasContext
    {
        // --- Per-entity clocks (transient, never serialized) -------------------------------------------
        // Keyed by entity, and an entity belongs to exactly one canvas, so these are the clocks of THIS
        // canvas's tree. Per canvas rather than per view because that is what gives them a death: the cell
        // goes when the canvas does, instead of accumulating a row per element the scene ever had.
        std::unordered_map<entt::entity, float>    HoverT;    // 0 = rest, 1 = hovered; eased each frame
        std::unordered_map<entt::entity, float>    TweenT;    // per-element tween playhead
        std::unordered_map<entt::entity, uint64_t> TweenSeen; // FrameIndex the tween was last evaluated on

        // --- Screens ----------------------------------------------------------------------------------
        // Which of THIS canvas's UIScreen sub-trees is current, and the hand-over running between two of
        // them.
        //
        // WHY THE PAIR AND NOT EITHER HALF. Per view, because one viewport navigating to Settings must not
        // move another view of the same scene — including the UI Editor's preview, which an author uses to
        // look at a screen the running game is not on. Per canvas, because a screen IS a sub-tree of one
        // canvas and the machine is seeded from that canvas's own UIScreenStackComponent: two canvases in
        // one view have disjoint screen NAMES, so a single machine cannot be current in both. The
        // re-seed below ("the current name does not exist here") is what turns that from a shared value
        // into an oscillation, since each canvas's walk finds the other's name foreign and overwrites it.
        //
        // The alternative — one screen per GAME, i.e. per process — was considered and refused: it is the
        // process-wide state this whole file exists to undo, and it cannot express the editor previewing a
        // screen while the viewport shows another, which is the ordinary authoring loop.
        std::string              Screen;                // current screen name
        std::string              ScreenFrom;            // the one handing over, while a transition runs
        std::vector<std::string> ScreenStack;           // history for BackScreen
        float                    ScreenT       = 1.0f;  // 0..1 progress of the transition (1 = idle)
        float                    ScreenSlidePx = 60.0f; // mirrored from the canvas's UIScreenStack
        float                    ScreenTime    = 0.25f;
        ECS::UIEasing            ScreenEasing  = ECS::UIEasing::CubicOut;
        bool                     ScreenBack    = false; // a Back transition slides the other way
        std::string              ScreenReq;             // requested by a button, applied at the walk's end
        bool                     ScreenReqBack = false;

        // The background sprite this cell last refused to draw, so an unresolvable handle is reported once
        // instead of once per frame. Cleared when the handle changes or resolves.
        Assets::AssetHandle WarnedBackground;

        // --- Overlays (Ю12) ---------------------------------------------------------------------------
        // Everything an overlay canvas remembers between frames, and the reason none of it is in the
        // component: two views of one scene may have the same context menu open in different places, and
        // opening one in the editor's preview must not dirty the level. Same line UIScreen already draws.
        // Meaningless — and untouched — for a canvas with no UIOverlayComponent.

        // Is this overlay showing? A closed overlay canvas draws nothing and elects nothing, which is not
        // the same as UICanvasData::Visible being false: Visible is the author saying "never", this is the
        // runtime saying "not now".
        bool OverlayOpen = false;

        // Screen-px displacement of the whole canvas root, so the authored tree lands where the overlay was
        // placed. Computed by PlaceOverlay from the origin and the canvas's measured content box; zero for
        // Modal and Toast, which have no origin (see UILayout.hpp).
        //
        // IT IS READ BY THE LAYOUT QUERIES TOO, not only by the walk. EnumerateCanvas / PickElement /
        // GetElementRect resolve rects "the same way RenderCanvas2D does" (UICanvasLayout.hpp), and a shift
        // applied by one and not the other is precisely the middle link that drops a property: the menu
        // would draw where it was placed and be clickable where it was authored.
        glm::vec2 OverlayShift{ 0.0f };

        // The trigger element this overlay was opened by, or entt::null for one opened by name alone. Kept
        // so a pinned tooltip can be re-placed when its trigger moves, and so a submenu knows which item of
        // the menu below owns it — closing that menu closes this one.
        entt::entity OverlayOpenedBy = entt::null;

        // One notification on screen: what it says, and how long it has left. The list is the VISIBLE
        // stack, at most UIOverlayData::ToastSlots long; everything else waits in OverlayToastPending.
        struct ToastItem
        {
            std::string Text;
            float       Remaining = 0.0f;
        };
        std::vector<ToastItem>  OverlayToastVisible;
        std::deque<std::string> OverlayToastPending;

        // Has the overflow of the pending queue already been reported since it last drained? A dropped
        // notification is worth a line, and a flood of them is worth exactly one line, not one per drop.
        bool OverlayToastOverflowReported = false;

        // --- Per-(canvas x view) binding overrides ---------------------------------------------------
        // A UIBinding reads this FIRST and the process-wide UIDataStore second, so a value that belongs to
        // one view's own state can reach an authored Text/Visible binding without being written anywhere a
        // second view can see it. It is what carries `overlay.text` into a tooltip and
        // `overlay.toast.N.text` / `.visible` into a toast slot.
        //
        // WHY NOT JUST WRITE THE GLOBAL STORE. Because the store is one per process and an open tooltip is
        // one per (canvas x view): two viewports of one scene would show each other's tooltip text, and the
        // editor's preview would overwrite the viewport's. That is the exact defect this whole file was
        // written to undo, and reintroducing it for a string is not a saving.
        UIDataStore Locals;
        // Style names this cell has already refused (Ю13): a UIStyleComponent naming a style the canvas's
        // theme does not declare. Per NAME rather than per ENTITY, because a mistyped style is normally on
        // the twenty elements that were duplicated from one another and the interesting fact is the name,
        // said once. Cleared when the canvas's theme handle changes, so the same typo is reported again
        // against a theme that might have declared it.
        std::unordered_set<std::string> WarnedStyles;
        // The theme handle WarnedStyles was accumulated against — see above.
        Assets::AssetHandle WarnedStylesTheme;
    };

    // WHICH STORE A BINDING ON THIS CANVAS READS, asked once so the two readers cannot drift.
    //
    // There are exactly two of them — the renderer's SampleBinding and the layout walk's
    // BindingHidesElement — and they must agree about every key, because UIIntrospection asserts that the
    // enumeration and the draw list describe the same frame. One asking the locals and the other not is the
    // middle link that drops a property in its purest form: a toast slot would be hidden in one walk and
    // drawn in the other.
    //
    // The locals WIN when they hold the key, and fall through otherwise, so a scene-wide gameplay value and
    // a per-view overlay value can coexist under different names without either shadowing the other.
    [[nodiscard]] inline const UIDataStore& BindingStore( const UICanvasContext* ctx, const std::string& key )
    {
        return ( ctx != nullptr && ctx->Locals.Has( key ) ) ? ctx->Locals : UIDataStore::Get();
    }

    // ONE VIEW — the other coordinate of the key, and the owner of every cell it has drawn.
    //
    // What is here rather than in the cell is what there is exactly ONE of per view no matter how many
    // canvases it draws: the pointer (one cursor elects one hot element across every canvas, which is what
    // makes an overlay canvas take the click away from the HUD under it), the drag it may be carrying, the
    // view's clock, and the Render2D backend its materials belong to.
    class UIViewContext
    {
    public:
        // --- Identity ---------------------------------------------------------------------------------
        // The registry this state describes, remembered so a view that is pointed at another scene starts
        // clean instead of reading its predecessor's entity ids. Compared by address and never dereferenced.
        // BeginUIFrame does the comparison and the Reset, so a host that swaps scenes needs no discipline;
        // a freed registry's address CAN be reused by the next one, which is why Reset() is also public.
        const entt::registry* Registry = nullptr;

        // --- This view's clock ------------------------------------------------------------------------
        // Wall-clock seconds at this view's previous frame, and the delta from it. Per view because two
        // views draw in the same frame: with one shared clock the second walk of every frame measured a
        // delta of ~0 and its hover eases, tweens and screen transitions stood still. It is advanced ONCE
        // per view frame (BeginUIFrame) and not once per canvas, or the same freeze reappears between the
        // canvases of a single view.
        // HasDrawn is a flag rather than a sentinel value in LastFrameTime, because the clock's epoch is the
        // first UI frame of the process: a "not yet" spelled as a negative time is indistinguishable from a
        // real reading taken in the first fraction of a second.
        bool     HasDrawn      = false;
        float    LastFrameTime = 0.0f;
        float    FrameDt       = 0.0f;
        uint64_t FrameIndex    = 0; // drives the tween's rewind-on-hide check

        // Is a frame of this view open — i.e. has BeginUIFrame run and EndUIFrame not yet? A walk outside
        // one is REFUSED by name rather than drawn, because the alternative is the silent freeze this whole
        // frame boundary exists to prevent: without a BeginUIFrame the clock never advances, FrameDt stays
        // 0, and every ease, tween and screen transition in the view stands still while the picture looks
        // entirely correct.
        bool FrameOpen = false;

        // Is this view an AUTHORING preview rather than a running UI? An authoring view draws every overlay
        // canvas of the scene as it was authored — open, unplaced — because an author who cannot see the
        // context menu they are building cannot build it. A running view shows an overlay only while its
        // state machine says it is open.
        //
        // IT IS A PROPERTY OF THE VIEW AND IT IS APPLIED TO THE CELLS. BeginUIFrame opens every overlay cell
        // when this is set, so the cell stays the single statement of "is this overlay showing" — read
        // identically by the walk that DRAWS and by the walk that ANSWERS QUESTIONS (EnumerateCanvas). A
        // second, parallel "...unless we are authoring" test in one of the two is how they come to describe
        // different frames.
        bool AuthoringPreview = false;

        // What AuthoringPreview said on this view's previous frame. Crossing between the two modes throws
        // the open set away, because it means something different on each side: Design opens every overlay
        // so it can be edited, and carrying that into Preview would start the game with every menu and
        // dialog already up. Kept here rather than in the host, because the host that toggles the mode is
        // not the one that draws the frame.
        bool AuthoringLastFrame = false;

        // Does this view drive the scene's SHARED animation playheads (UIAnimComponent::Data::Time)? That
        // one clock lives in the component on purpose — the Sequencer scrubs it — so it is scene state, not
        // view state, and exactly one view may advance it. A second view advancing it too runs every UIAnim
        // clip at double speed. The authoring preview sets this false; the viewport / game keeps it true.
        bool DrivesSceneAnimation = true;

        // --- Hit testing ------------------------------------------------------------------------------
        // The frame elects a single HOT element (last writer in draw order = topmost) and controls compare
        // against the PREVIOUS frame's winner, the same one-frame deferral ImGui uses. The election spans
        // every canvas of the frame and is handed over once, in EndUIFrame — doing it per walk gave the
        // LAST canvas drawn the whole answer, so an overlay canvas erased the HUD's hot element merely by
        // existing.
        entt::entity Hot     = entt::null; // resolved last frame: what the controls react to now
        entt::entity HotNext = entt::null; // being elected during this frame's walks
        Rect         HotNextRect{};
        bool         PrevDown = false; // for the press edge (UIInput only carries held + release)
        UIDragState  Drag;

        // Every focusable control the frame drew, in draw order across every canvas — Tab advances through
        // this one list, so focus can leave a HUD canvas and enter an overlay. Rebuilt each frame.
        std::vector<entt::entity> Focusables;

        // --- The frame's viewport, stated ONCE --------------------------------------------------------
        // Where this view draws, in pixels. It used to be a parameter of every RenderCanvas2D call, so a
        // frame could hand two canvases two different viewports and nothing said so — and the overlay
        // update, which runs after the walks and has to place a box inside the view, would have had to be
        // handed a third copy of the same number. One value per view frame, set by BeginUIFrame and read by
        // everything else, is the only shape in which those cannot disagree.
        Rect ViewportPx{};

        // --- Overlays (Ю12) ---------------------------------------------------------------------------

        // The open overlay canvases of this view, OUTERMOST FIRST. It is a stack because a submenu is a
        // ContextMenu overlay opened by an item of the menu below it: Escape closes the innermost, a click
        // outside closes back to whichever entry owns the click, and closing an entry closes everything
        // opened after it. Per view and not per canvas, because the pointer is per view.
        std::vector<entt::entity> OverlayStack;

        // The hover trigger the pointer is currently resting on, and for how long. A hover overlay opens
        // when this reaches its UIOverlayData::OpenDelay; moving to another trigger — or to nothing —
        // restarts the clock, which is what makes a delay a delay rather than an accumulator.
        entt::entity OverlayHoverTrigger = entt::null;
        float        OverlayHoverHeld    = 0.0f;

        // The one open tooltip of this view, if any. A tooltip is deliberately NOT on the stack above: it
        // captures nothing, so Escape and click-outside have no business reaching it, and there can only
        // ever be one because there is only one pointer.
        entt::entity OverlayTooltip = entt::null;

        // A hover trigger whose overlay was DISMISSED — by Escape, or by a press outside it — while the
        // pointer was still resting on the trigger. Without this the dismissal cannot be seen: the hover
        // clock is still satisfied on the very next frame and reopens what was just closed, so Escape looks
        // like it does nothing. Cleared the moment the pointer is on a different trigger (or none), which
        // is the user's way of saying they are done with that dismissal.
        entt::entity OverlayHoverSuppressed = entt::null;
        bool         PrevRightDown          = false; // the right-button press edge, as PrevDown is for the left

        // --- Walk-local -------------------------------------------------------------------------------
        // The tint of the element being drawn, multiplied into its colours so Opacity/Color tweens reach
        // every control. Saved and restored by the recursion, so it enters and leaves each walk at 1.
        glm::vec4 Tint{ 1.0f };

        // --- UI materials (Ю11) -------------------------------------------------------------------------
        // Where this view's UI materials come from — the Render2D backend that will draw the list. It is
        // a VIEW's and not the process's because a UI material owns a pipeline, a pipeline is compiled
        // against ONE framebuffer's render pass, and two viewports have two targets. A source reached
        // through a global would hand the second viewport pipelines built against the first one's pass.
        // See UIMaterialSource.hpp for why it is an interface.
        //
        // Null means this walk has no GPU backend behind it — a unit test, or a host that forgot to wire
        // one. It is NOT a quiet "no materials today": an element whose slot is set draws its ordinary
        // fill and the view says so ONCE, naming the element, because a panel that silently loses its
        // material looks exactly like a panel nobody put a material on.
        IUIMaterialSource* Materials = nullptr;

        // The material handle this view last drew without a backend, so that report happens once.
        Assets::AssetHandle WarnedMaterial;

        // --- UI render textures (Ю16) ------------------------------------------------------------------
        // Where this view's render-texture elements get their worlds from. A VIEW's and not the
        // process's for a reason one step stronger than the material cache's: the backend behind this
        // owns renderer SLOTS, six of them for the whole process, and it hands one back by destroying
        // the capture that held it. It learns which captures are still wanted from the walks of the view
        // it belongs to — so a source shared between two views would see one view's walk as the other
        // view's abandonment and destroy a picture that is on screen.
        //
        // Null means this walk has no GPU backend behind it — a unit test, or a host that forgot to wire
        // one. It is NOT a quiet "no worlds today": the element draws the magenta error fill, because an
        // element whose entire job is a picture and which silently draws nothing looks exactly like an
        // element nobody finished authoring. Said ONCE per view, naming the element.
        IUIRenderTextureSource* RenderTextures = nullptr;

        // The element this view last drew without a backend, so that report happens once.
        entt::entity WarnedRenderTexture = entt::null;

        // --- The (canvas x view) table ----------------------------------------------------------------

        // This view's cell for @p canvas, created empty on first use. The only way to obtain one, so a cell
        // can never be paired with a view that does not own it.
        UICanvasContext& CanvasState( entt::entity canvas )
        {
            return m_Canvases[canvas];
        }

        // The cell for @p canvas if this view has ever drawn it, else nullptr. Nullptr is a MEANINGFUL
        // answer — "this view has not drawn that canvas" — and callers that read state without drawing
        // (the UI Debugger's probe) must be able to tell it from a canvas sitting on its first screen.
        [[nodiscard]] const UICanvasContext* FindCanvasState( entt::entity canvas ) const
        {
            const auto it = m_Canvases.find( canvas );
            return it == m_Canvases.end() ? nullptr : &it->second;
        }

        [[nodiscard]] std::size_t CanvasStateCount() const
        {
            return m_Canvases.size();
        }

        // --- Lifetime ---------------------------------------------------------------------------------
        //
        // WHO OWNS A CELL: the view, by value, and nothing else holds a pointer into the table. A view is a
        // member of its host (EditorUIPass per open document, UIEditorPanel per window, RuntimeLayer for the
        // player), so closing the view destroys every cell it had — there is no registry keyed by address to
        // forget, which is what UIProbeRegistry has to do and what makes it need an explicit Forget().
        //
        // WHEN A CELL DIES INSIDE A LIVING VIEW: the frame it stops being a canvas. entt RECYCLES entity
        // ids, so a cell left behind by a destroyed canvas is not merely dead weight — the next canvas the
        // scene creates can be handed that id and would inherit a stranger's screen stack and hover clocks.
        // That is the twin of "a hidden preview still owns its renderer slot" (RENDERER_FRAME_STATE.md), and
        // it is closed the same way: by an owner that releases rather than by hoping nobody notices.
        // RetireDeadCanvases runs once per frame from BeginUIFrame over a table with as many rows as the
        // scene has canvases.
        void RetireDeadCanvases( const entt::registry& reg )
        {
            for ( auto it = m_Canvases.begin(); it != m_Canvases.end(); )
                it = ( reg.valid( it->first ) && reg.has<ECS::UICanvasComponent>( it->first ) )
                          ? std::next( it )
                          : m_Canvases.erase( it );

            // The overlay stack is an INDEX INTO that table, so it dies on the same frame the cells do.
            // An index that outlives what it points at is the identical defect one level up: a destroyed
            // menu would still be "open", and Escape would pop an entity nobody can ask anything about.
            const auto gone = [&reg]( entt::entity c )
            { return !reg.valid( c ) || !reg.has<ECS::UICanvasComponent>( c ); };
            OverlayStack.erase( std::remove_if( OverlayStack.begin(), OverlayStack.end(), gone ),
                                OverlayStack.end() );
            if ( OverlayTooltip != entt::null && gone( OverlayTooltip ) )
                OverlayTooltip = entt::null;
        }

        // Forget everything about the scene drawn so far. BeginUIFrame calls this itself when it notices the
        // registry changed; a host that swaps scenes behind the same address calls it directly.
        void Reset()
        {
            Registry = nullptr;
            m_Canvases.clear();
            Hot         = entt::null;
            HotNext     = entt::null;
            HotNextRect = Rect{};
            PrevDown    = false;
            Drag        = UIDragState{};
            Focusables.clear();
            Tint           = glm::vec4( 1.0f );
            WarnedMaterial = Assets::AssetHandle{};
            ViewportPx     = Rect{};
            OverlayStack.clear();
            OverlayTooltip         = entt::null;
            OverlayHoverSuppressed = entt::null;
            OverlayHoverTrigger    = entt::null;
            OverlayHoverHeld       = 0.0f;
            PrevRightDown          = false;
        }

    private:
        std::unordered_map<entt::entity, UICanvasContext> m_Canvases;
    };
} // namespace Desert::UI
