#pragma once

#include <UI/UICanvasContext.hpp>
#include <UI/UICanvasRenderer2D.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <UI/UITree.hpp>

#include <string>
#include <vector>

// OVERLAYS: WHEN AN OVERLAY CANVAS IS OPEN, WHERE IT IS, AND WHAT IT SAYS.
//
// The four features this file serves — tooltip, context menu, modal, toast — are not four subsystems.
// Each is a canvas with a higher UICanvasData::SortOrder, drawn after the rest of the scene's canvases,
// and Ю4 already made that mean "it takes the pointer from what it covers": the hot election is one per
// VIEW per frame and the last writer in draw order wins. Nothing here re-implements stacking, clipping,
// hit testing or event routing.
//
// WHAT IS LEFT, AND IT IS ALL THIS FILE IS:
//
//   1. a state machine — which overlays are open, opened by what, closed by what;
//   2. a placement — where the box goes so it does not leave the view (UI::PlaceOverlay, UILayout.hpp);
//   3. one string channel — the per-(canvas x view) locals a UIBinding reads, which is how a runtime
//      tooltip line or a toast notification reaches an authored Text element without ever being written
//      into the scene;
//   4. a bounded toast queue.
//
// MODALITY IS NOT A FLAG. A modal canvas paints a scrim over the whole view and is ELECTED behind its own
// content, so for every point the dialog does not cover the frame's single hot element is the modal canvas
// itself. A button underneath compares against that election like every other control and finds it is not
// the winner; a press routes to the scrim, which has no listener, and stops. There is no second rule that
// says "if a modal is open, ignore clicks" — there is nowhere for such a rule to live, because the
// election already answers the question once for the whole view.
namespace Desert::UI
{
    // The overlay policy of @p canvas, or nullptr when it is not an overlay. Reads the component; every
    // caller in the engine goes through this so "is this an overlay" is one question with one answer.
    [[nodiscard]] const UIOverlayData* OverlayDataOf( IUITree& tree, NodeId canvas );

    // The overlay canvas named @p name. REFUSES when there is none and when there is more than one, with
    // the count in the message — the same rule as UI::SoleCanvas, for the same reason: picking one of two
    // identically named overlays is a silent wrong answer, and the author's typo would surface as "the
    // wrong menu opens" months later.
    [[nodiscard]] Common::ResultStr<NodeId> OverlayByName( IUITree& tree, const std::string& name );

    // The keys the overlay update publishes into a canvas's per-view locals, and the ONLY contract between
    // the runtime and an authored overlay's chrome. A Text element inside a tooltip carries a UIBinding to
    // kOverlayTextKey; a toast slot binds kOverlayToastText( i ) and kOverlayToastVisible( i ).
    inline constexpr const char* kOverlayTextKey = "overlay.text";

    [[nodiscard]] std::string OverlayToastTextKey( int slot );
    [[nodiscard]] std::string OverlayToastVisibleKey( int slot );

    // Run one frame of the overlay machine for @p view, on the election the frame just made. Called by
    // EndUIFrame and by nothing else: it reads UIViewContext::HotNext, which only exists between the last
    // canvas's walk and the hand-over.
    //
    // @p input is never null here — a walk with no input is an authoring walk, and an authoring walk draws
    // every overlay as authored instead of running a state machine over a pointer it does not have.
    void UpdateOverlays( UIViewContext& view, IUITree& tree, const UIInput& input );

    // Close every open overlay of @p view, e.g. because the scene is being torn down or play stopped.
    void CloseAllOverlays( UIViewContext& view, IUITree& tree );
} // namespace Desert::UI
