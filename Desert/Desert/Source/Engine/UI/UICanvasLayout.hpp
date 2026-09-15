#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/Render2D/ClipRegion2D.hpp>
#include <Engine/UI/UILayout.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// Layout QUERIES over a canvas tree: where does an element end up on screen, and what is under the cursor.
//
// These used to live in UICanvasRenderer.{hpp,cpp} next to the ImGui draw path. That draw path is gone —
// the engine ships one canvas renderer (UICanvasRenderer2D, into a DrawList2D) — but the queries are not
// drawing and never were: the editor needs them to click-select a UI element in the viewport, to put a
// selection marquee around it and to convert an on-screen size back into design-space UILayout offsets.
// Splitting them out is also what takes ImGui out of Engine/ (a layering rule the old file broke).
//
// THE RELATION THAT MATTERS: these resolve a rect the same way RenderCanvas2D does — the canvas scale
// mode, the safe-area inset, the aspect fitter, the content-size fitter and auto-layout group placement.
// If the two ever drift, a click lands on nothing while the element is plainly on screen. Anything added
// to the renderer's rect resolution belongs here too.
namespace Desert::UI
{
    // --- WHICH CANVAS. The question every one of these used to answer by itself, and always the same way --
    //
    // `*reg.view<UICanvasComponent>().begin()` — the first canvas entt happens to hand out — stood in three
    // places (the renderer's walk, the layout queries below, the editor's element factory) and it is not a
    // choice, it is a coincidence: entt's iteration order is a property of the pool, so "the first canvas"
    // is whichever one the scene file happened to create first. Everything downstream inherited it. A second
    // canvas was silently never drawn, never picked and never measured, which is why HUD and menu could not
    // be separated, why an overlay and a world-space canvas could not coexist, and why a prefab could not
    // carry its own canvas. The UI Editor could not preview the second canvas either and had to refuse by
    // name (U7-2 left that refusal in place, pointing here).
    //
    // So the canvas is now an ARGUMENT everywhere below and in RenderCanvas2D, and these four functions are
    // the only ways to obtain one. None of them can silently pick a winner:
    //
    //   CanvasOf     derives the answer from an element that already names it — its own canvas ancestor.
    //                This is what the editor uses, and it is exact rather than lucky.
    //   CanvasCount  counts. Counting is not electing, and it is what lets a host say "no canvas yet, offer
    //                to create one" and "more than one, ask which" as two different sentences.
    //   SoleCanvas   the answer for a host that must name exactly ONE — the UI Editor's element factory,
    //                which has to put a new element somewhere. It REFUSES when there is none and when
    //                there is more than one, with the count in the message — because "there are two and I
    //                chose one of them" is exactly the silent wrong answer the contract forbids.
    //   CanvasesInDrawOrder
    //                ALL of them, ordered. This is what a view that DRAWS uses, and it is why the two
    //                drawing hosts (the viewport's EditorUIPass and the runtime) no longer refuse a scene
    //                with two canvases: refusing was the honest answer while a view could hold the state
    //                of only one canvas, and Ю4 removed that limit. A HUD and a pause menu are two
    //                canvases of one level, and the four overlay features (tooltip, context menu, modal,
    //                toast) are each an extra canvas drawn over the rest.

    // The canvas @p e belongs to: @p e itself when it carries a UICanvasComponent, otherwise the nearest
    // ancestor that does. entt::null when @p e is not under a canvas at all (a plain 3D entity, or a UI
    // element that has not been parented yet). Cycle-safe: the walk is bounded by the entity count.
    [[nodiscard]] entt::entity CanvasOf( entt::registry& reg, entt::entity e );

    // How many UICanvasComponents this scene holds.
    [[nodiscard]] std::size_t CanvasCount( entt::registry& reg );

    // The scene's ONE canvas. Refuses, by name and with the count, when there is not exactly one.
    [[nodiscard]] Common::ResultStr<entt::entity> SoleCanvas( entt::registry& reg );

    // Every canvas of the scene, in the order a view must draw them: ascending UICanvasData::SortOrder,
    // and within one SortOrder the order the scene created them (which is the order the file lists them).
    // Later in the list = drawn later = on top, both for pixels and for the pointer, since the hot election
    // keeps the last writer.
    [[nodiscard]] std::vector<entt::entity> CanvasesInDrawOrder( entt::registry& reg );

    // --- The visibility axis, asked once ------------------------------------------------------------
    // Three places resolve an element's rect — the renderer's walk, the editor's pick, the editor's
    // marquee — and a fourth measures a container's content for the size fitter. All four have to agree
    // on WHICH CHILDREN EXIST, because a child counted by the layout and not by the pick is an element
    // drawn where nothing can click it, which is this project's recurring defect shape. So the two
    // questions are asked through these two functions and nowhere else.

    // Does @p e occupy a slot in its parent's auto-layout group? Only ECS::UIVisibility::Collapsed drops
    // out; Hidden keeps its slot, and that difference IS the layout axis. An element with no UILayout has
    // nothing to say and takes its slot.
    [[nodiscard]] bool TakesLayoutSpace( entt::registry& reg, entt::entity e );

    // Is @p e drawn at all — and therefore hit-testable at all? False for Hidden and Collapsed, both of
    // which take their whole sub-tree with them.
    [[nodiscard]] bool IsElementVisible( entt::registry& reg, entt::entity e );

    // --- ONE WALK, AND EVERY QUERY BELOW IS A READ OF IT ---------------------------------------------
    //
    // This file used to hold TWO near-identical recursions (PickRecurse, RectRecurse) and a third lived in
    // the renderer, all resolving the same rect from the same anchors. Two of the three had already drifted
    // apart from the renderer by the time this was written, in exactly the direction the note at the top of
    // this file warns about: neither applied a ScrollView's scroll offset to its children, and neither
    // intersected the inherited clip, so a row scrolled out of a list was picked at the position it would
    // have had unscrolled, and a row clipped away entirely was still pickable. Both are fixed by there
    // being one walk instead of three.
    //
    // The renderer's own DrawElement is still a separate recursion — it draws, and it is not this file's to
    // own — so exactly ONE relation is left to hold rather than three, and it is asserted rather than
    // described: `Desert/Tests/Engine/UIIntrospection` hides each element the enumeration calls SKIPPED and
    // requires the real walk's draw list to come out byte-identical.

    // Why an element the walk reached was not drawn. `AncestorSkipped` means the walk never reached it at
    // all — one of its ancestors stopped first — which is why it is a separate value from the self-causes:
    // a count that folded them together could not tell "forty hidden elements" from "one hidden panel".
    enum class UISkipCause : std::uint8_t
    {
        None,             // drawn
        SelfHidden,       // UIVisibility::Hidden on this element (keeps its layout slot)
        SelfCollapsed,    // UIVisibility::Collapsed on this element (drops its slot in a layout group)
        BindingHidden,    // a UIBinding with target Visible resolved to false in the UI data store
        ScreenNotCurrent, // a UIScreen sub-tree that is neither the current screen nor the one leaving
        OutsideWindow,    // a row of a UIListView outside the window this frame walks (Ю17)
        AncestorSkipped,  // an ancestor stopped for one of the reasons above

        // The register's own end, so nothing that is sized by it can be sized by a typed number instead.
        Count
    };

    [[nodiscard]] const char* UISkipCauseName( UISkipCause cause );

    // One element of a canvas, as the walk sees it. Produced in DRAW ORDER (depth first, a parent before
    // its children, siblings in RelationshipComponent order), which is also the order the hot element is
    // elected in — so `Order` answers "what is on top of what" without anybody re-deriving it.
    struct UIElementNode
    {
        entt::entity Entity = entt::null;
        entt::entity Parent = entt::null;
        int          Depth  = 0;  // 0 = a direct child of the canvas
        int          Order  = -1; // index among the DRAWN elements; -1 when this one is not drawn

        bool         Drawn   = false;
        UISkipCause  Cause   = UISkipCause::None;
        entt::entity CauseBy = entt::null; // who stopped it: itself, or the ancestor that did

        // The element's own rect, BEFORE its render transform — the space its UILayout offsets are written
        // in, the same rect GetElementRect reports. False when the element has no rect at all: a Collapsed
        // child of an auto-layout group is given no slot, so there is no position to report for it.
        bool RectValid = false;

        // The rect above is this element's OWN — resolved from its anchors, or handed to it by a parent
        // auto-layout group. False for an element with no UILayoutComponent, which simply inherits its
        // parent's box and which the pointer therefore cannot land on: it occupies no area of its own.
        bool      OwnRect = false;
        Rect      RectPx{};
        glm::mat3 Xform{ 1.0f }; // its own transform composed inside its ancestors'
        Rect      ScreenPx{};    // the axis-aligned box RectPx occupies on screen after Xform

        // THE CLIP THIS ELEMENT INHERITED, as the region — not as a scissor box. The field was a Rect for
        // exactly as long as a clip WAS a scissor; Ю9 made a rotated clipper cut its own quadrilateral on
        // the CPU, so a box can no longer say where the pixels went and a box here would be the middle link
        // that drops a property: PickElement reads this, the draw list cuts by the region, and the two would
        // disagree in the corners of every rotated clipper. Always Bounded — the walk's outermost level is
        // the viewport — so "unclipped" is not one of its states and an empty region is distinguishable
        // from a large one.
        Graphic::Render2D::ClipRegion2D ClipRegion{};

        Rect VisiblePx{}; // ScreenPx ∩ ClipRegion's BOX ∩ the viewport — the pixels it may own, box half only

        // Drawn, but no pixel of it can land: scrolled out of its list, cut away by a rotated clipper, or
        // off the viewport. The walk does NOT cull these — it records their geometry and the clip throws it
        // away — so this is the column that answers "why can I not see it" for an element that is neither
        // hidden nor mispositioned. Unlike VisiblePx this DOES account for the oblique half-planes, because
        // the box of an element a rotated clipper removed entirely is not empty.
        bool Clipped = false;

        bool           TakesSlot     = true; // counted by a parent auto-layout group (Collapsed drops out)
        ECS::UIHitTest HitTest       = ECS::UIHitTest::All;
        bool           ElectsSelf    = false; // may the pointer STOP here — own value narrowed by its ancestors'
        bool           ClipsChildren = false;
    };

    // Walk @p canvas into @p out. The rect resolution is the renderer's: canvas scale mode, safe area,
    // aspect fitter, content-size fitter, auto-layout group placement, render transform, scroll offset and
    // the clip chain.
    //
    // @p ctx IS WHAT MAKES THIS A RUNTIME ANSWER RATHER THAN AN AUTHORING ONE, and passing nullptr is a
    // deliberate second mode, not a degraded one. Two of the walk's skip reasons are properties of a VIEW
    // and not of the scene — which screen a view has navigated to, and what a gameplay data binding
    // currently says — so a host that has no view cannot be told about them. The editor's click-select is
    // such a host: it must be able to pick an element of a screen the game is not on, because that is when
    // an author is editing it. With a context, both reasons are honoured and the counts describe what was
    // actually drawn.
    //
    // Refuses (and leaves @p out empty) when @p canvas is not a canvas of @p reg, or is not Visible — the
    // same three distinguishable refusals the queries below already had.
    NO_DISCARD Common::BoolResultStr EnumerateCanvas( entt::registry& reg, entt::entity canvas,
                                                      const Rect& viewportPx, std::vector<UIElementNode>& out,
                                                      const struct UICanvasContext* ctx = nullptr );

    // Does a UIBinding with target Visible currently say NO for @p e? The runtime walk asks the same
    // question through the same store and skips the element's whole sub-tree when the answer is yes.
    //
    // @p ctx is the (canvas x view) cell, because a binding reads that cell's locals before the process-wide
    // store (UI::BindingStore). Passing nullptr asks the global store alone, which is the honest answer for a
    // caller that has no view — and the same nullptr that already means "no view" everywhere else here.
    [[nodiscard]] bool BindingHidesElement( entt::registry& reg, entt::entity e,
                                            const struct UICanvasContext* ctx = nullptr );

    // In-scene UI editing (viewport WYSIWYG). Returns the topmost element of @p canvas whose resolved rect
    // contains `pointPx`, or entt::null. `viewportPx` must be the SAME rect the canvas was drawn into so
    // hit-testing matches what is on screen. A host with several canvases asks each one and keeps the last
    // hit — which is what makes an overlay in front of a HUD pickable at all.
    [[nodiscard]] entt::entity PickElement( entt::registry& reg, entt::entity canvas, const glm::vec2& pointPx,
                                            const Rect& viewportPx );

    // Resolves the on-screen rect of @p target (an element of @p canvas, or @p canvas itself) under the same
    // layout the renderer uses — for the selection marquee and the drag handles. false if @p target is not
    // in that canvas's tree, which is now a MEANINGFUL false: asking the wrong canvas is a caller error and
    // no longer silently answers about somebody else's element.
    //
    // `out` is the element's rect BEFORE its render transform, because that is the space its UILayout
    // offsets are written in — a caller that resized a rotated element from a transformed rect would
    // write back offsets that had been through the rotation twice. @p outXform (optional) is the
    // accumulated transform — its own composed inside its ancestors' — that maps that rect onto the
    // screen, so a caller that wants to DRAW something over the element (a marquee, handles) has both
    // halves and neither has to guess. Identity when nothing in the chain is transformed.
    [[nodiscard]] bool GetElementRect( entt::registry& reg, entt::entity canvas, entt::entity target,
                                       const Rect& viewportPx, Rect& out, glm::mat3* outXform = nullptr );

    // @p canvas's current uniform scale (design px -> screen px) for the given viewport, per its scale mode
    // (1 in Stretch). The editor divides on-screen sizes by this when writing UILayout offsets so a value it
    // computes from GetElementRect round-trips instead of being scaled twice.
    //
    // IT REFUSES RATHER THAN ANSWERING 1. This used to elect a canvas itself and return 1 when it found none
    // — and 1 is a perfectly plausible scale (it is what Stretch gives), so a caller could not tell a real
    // answer from "there was nothing to measure" and would write offsets scaled by the wrong factor.
    [[nodiscard]] Common::ResultStr<float> CanvasScale( entt::registry& reg, entt::entity canvas,
                                                        const Rect& viewportPx );
} // namespace Desert::UI
