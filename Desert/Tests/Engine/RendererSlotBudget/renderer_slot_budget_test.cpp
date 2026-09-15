// "A USER SURFACE MAY TAKE THE LAST RENDERER SLOT. BACKGROUND WORK MAY NOT."
//
// WHAT IS AT STAKE. A Graphic::SceneRenderer that finds every slot taken does not fail — it records into
// slot 0 and shares the main viewport's per-frame state. The symptom is "my preview moved when I moved
// the scene camera", there is no error message, and Docs/RENDERER_FRAME_STATE.md exists because it cost
// days. Six slots, no seventh, so the only question is who yields; this file pins the answer.
//
// WHY A TEST AND NOT A FRAME. Both refusals are invisible until six renderers are alive AT ONCE and STAY
// alive. Measured on this machine through the editor's control channel: opening five material documents
// in a row touches 6/6 for a single frame and falls back to 3/6 within eight seconds, because a document
// hands its renderer back when its tab is not the focused one. A sustained shortage needs several scene
// VIEWS, which live behind the Scenes menu, and the control channel runs palette commands rather than
// clicking menus. So the state a frame would have to photograph cannot be held long enough to photograph
// — which is precisely the argument RendererSlotPool's own header makes for being a pure, device-free
// class: a lease nobody can see from a picture has to be drivable by a test.
//
// WHAT WOULD MAKE THIS RED, and each is a real mistake:
//   * giving background captures the same entitlement as a surface the person clicked (they would take
//     the sixth slot to render the consolation picture for not having a sixth slot);
//   * making the Details preview yield the last slot, so clicking an entity stops previewing it while
//     something nobody asked for keeps a renderer;
//   * a second copy of the arithmetic appearing at one of the THREE call sites and drifting from this one.
//
// MOVED IN Ю16, from Editor/Widgets/PreviewSlotBudget.hpp to Engine/Core/RendererSlotBudget.hpp, and the
// move is the finding rather than tidying: the third caller is a UI element that hosts a world, which
// RuntimeLayer walks inside the PACKAGED GAME (UI::RenderCanvas2D), where nothing under `Editor/` is
// linked. A rule that answers only in the editor is a rule plus a silent second copy in the process that
// ships. The suite moved with it and is device-free for the same reason it always was.

#include <Engine/Core/RendererSlotBudget.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace
{
    using Desert::Engine::RendererSlotBudget::Demand;
    using Desert::Engine::RendererSlotBudget::MayClaim;

    // The engine's own number, spelled here so a change to it shows up as a decision rather than as a
    // silently different test. EngineContext::kMaxRendererSlots is not included: it drags in the whole
    // renderer, and this suite is deliberately device-free.
    constexpr uint32_t kSlots = 6;
} // namespace

// --- The two entitlements, at the boundary that separates them ------------------------------------

TEST( RendererSlotBudget, AUserSurfaceMayTakeTheLastSlot )
{
    // Nothing open but the viewport, through to one slot left: always yes.
    for ( uint32_t live = 0; live < kSlots; ++live )
        EXPECT_TRUE( MayClaim( Demand::UserSurface, live, kSlots ) )
             << "a surface the person opened was refused with " << live << " of " << kSlots
             << " in use. Refusing it means refusing the thing that WAS asked for in favour of something "
                "that was not.";
}

TEST( RendererSlotBudget, NobodyMayClaimWhenThereIsNothingLeft )
{
    // The one case both agree on, and the only case where a claim would silently share slot 0.
    EXPECT_FALSE( MayClaim( Demand::UserSurface, kSlots, kSlots ) );
    EXPECT_FALSE( MayClaim( Demand::Background, kSlots, kSlots ) );

    // And past it, which happens the moment something has already overflowed.
    EXPECT_FALSE( MayClaim( Demand::UserSurface, kSlots + 3, kSlots ) );
    EXPECT_FALSE( MayClaim( Demand::Background, kSlots + 3, kSlots ) );
}

TEST( RendererSlotBudget, BackgroundWorkKeepsOneSlotInReserve )
{
    // Room to spare: yes.
    for ( uint32_t live = 0; live + 1 < kSlots; ++live )
        EXPECT_TRUE( MayClaim( Demand::Background, live, kSlots ) ) << "refused with " << live << " in use";

    // Exactly one slot left: no. THIS is the row the task's condition is about — a thumbnail capture may
    // never be the thing that takes the last one.
    EXPECT_FALSE( MayClaim( Demand::Background, kSlots - 1, kSlots ) )
         << "a background capture claimed the LAST free renderer slot. The next surface the person opens "
            "then shares slot 0 with the main viewport and borrows its camera, silently — and the capture "
            "it was taken instead of is the picture that surface would have shown in the meantime.";
}

// --- The RELATION between them, which is what makes them two rules rather than one copied twice ----

TEST( RendererSlotBudget, TheTwoEntitlementsDifferAndOnlyInTheDirectionIntended )
{
    // 1. Background is never MORE entitled than a user surface. Assert it over the whole range rather
    //    than at the boundary: an inverted comparison passes a boundary check and fails this.
    for ( uint32_t live = 0; live <= kSlots + 2; ++live )
    {
        if ( MayClaim( Demand::Background, live, kSlots ) )
        {
            EXPECT_TRUE( MayClaim( Demand::UserSurface, live, kSlots ) )
                 << "at " << live << " of " << kSlots
                 << " in use, background work may claim a slot and a user surface may not. The entitlements "
                    "are the wrong way round.";
        }
    }

    // 2. And they are not the SAME rule wearing two names: there is at least one occupancy where the two
    //    answers differ. Without this, replacing the whole file with `return live < max` would pass
    //    everything above — the vacuous-census failure, one level down.
    bool differSomewhere = false;
    for ( uint32_t live = 0; live <= kSlots + 2; ++live )
    {
        differSomewhere = differSomewhere || ( MayClaim( Demand::UserSurface, live, kSlots ) !=
                                               MayClaim( Demand::Background, live, kSlots ) );
    }
    EXPECT_TRUE( differSomewhere )
         << "UserSurface and Background answer identically at every occupancy, so the distinction this "
            "header exists to make has been erased. One of the two call sites is now taking a slot it "
            "should not, or yielding one it should have.";
}

// --- Degenerate inputs, because the count comes from a live system --------------------------------

TEST( RendererSlotBudget, AZeroSlotPoolGrantsNothingRatherThanWrappingAround )
{
    // `live + reserved < max` with unsigned arithmetic: the failure to avoid is an underflow that turns
    // "no slots at all" into "everyone may claim". There is no such pool today, and a rule that is only
    // correct because of a value elsewhere is a rule waiting for that value to change.
    EXPECT_FALSE( MayClaim( Demand::UserSurface, 0, 0 ) );
    EXPECT_FALSE( MayClaim( Demand::Background, 0, 0 ) );
    EXPECT_FALSE( MayClaim( Demand::Background, 0, 1 ) )
         << "a one-slot pool has nothing to reserve AND nothing to give: the viewport is that slot.";
    EXPECT_TRUE( MayClaim( Demand::UserSurface, 0, 1 ) );
}

// Compile-time as well as run-time: the callers use this in an `if`, and a constexpr rule that can be
// evaluated at compile time is one nobody can accidentally make expensive or stateful.
static_assert( MayClaim( Demand::UserSurface, 5, 6 ) );
static_assert( !MayClaim( Demand::Background, 5, 6 ) );
static_assert( !MayClaim( Demand::UserSurface, 6, 6 ) );

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
