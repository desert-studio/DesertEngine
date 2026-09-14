// "AN OVERLAY IS A CANVAS ON TOP, AND BEING ON TOP MEANS TAKING THE POINTER."
//
// The four features this suite covers — tooltip, context menu, modal, toast — were built on the seam Ю4
// left: a view holds one cell per (canvas x view), a scene's canvases are drawn in authored Sort Order,
// and the hot element is elected ONCE per view per frame with the last writer winning. That last sentence
// is the whole of modality, and `UICanvasContextPair.TheCanvasDrawnLastTakesThePointerFromTheOneBelowIt`
// already asserted it before any of this existed.
//
// SO WHAT IS ASSERTED HERE IS THE RELATIONS, not the features:
//
//   * a press that misses a modal does NOT reach the element under it — and, the control that matters,
//     the identical press DOES reach it when the modal is closed. Without that second half, "the modal
//     swallows everything" and "nothing ever reaches anything" pass the same test;
//   * a tooltip opened against the right edge FLIPS to the other side of the pointer and lies entirely
//     inside the view — with the control that the same tooltip in the middle of the view does not flip;
//   * a toast leaves on its own clock, the visible stack is bounded by the authored slots and the queue
//     behind it is bounded too;
//   * a context menu closes on Escape, and closes on a press outside itself;
//   * a submenu is the same mechanism nested, so closing the parent closes it;
//   * what the walk DRAWS and what the layout queries ANSWER agree about an overlay's placement — the
//     middle link that would otherwise draw a menu in one place and let it be clicked in another.
//
// Every one is written so that removing the mechanism reddens it rather than merely changing a number;
// the mutations that were run are in the report.

#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIMaterialSource.hpp>
#include <Engine/UI/UIOverlay.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// The renderer resolves sprites, fonts, icons and video through these. Every one of them owns GPU objects,
// and every draw helper already copes with the service being absent. The service METHODS below can then
// never run; each fails outright rather than returning a plausible value.
namespace Desert::Runtime
{
    TextureService* ResourceRegistry::GetTextureService()
    {
        return nullptr;
    }
    ImageService* ResourceRegistry::GetImageService()
    {
        return nullptr;
    }
    FontService* ResourceRegistry::GetFontService()
    {
        return nullptr;
    }
    IconService* ResourceRegistry::GetIconService()
    {
        return nullptr;
    }
    AnimatedImageService* ResourceRegistry::GetAnimatedImageService()
    {
        return nullptr;
    }
    VideoService* ResourceRegistry::GetVideoService()
    {
        return nullptr;
    }

    Graphic::Texture2D* TextureService::Get( const Assets::AssetHandle& ) const
    {
        ADD_FAILURE() << "TextureService::Get reached with no texture service";
        return nullptr;
    }
    Graphic::Image* ImageService::Resolve( const ImageHandle& ) const
    {
        ADD_FAILURE() << "ImageService::Resolve reached with no image service";
        return nullptr;
    }
    Graphic::Image2D* AnimatedImageService::Resolve( const Assets::AssetHandle& )
    {
        ADD_FAILURE() << "AnimatedImageService::Resolve reached with no animated image service";
        return nullptr;
    }
    Graphic::Image2D* VideoService::Resolve( uint64_t )
    {
        ADD_FAILURE() << "VideoService::Resolve reached with no video service";
        return nullptr;
    }
    Font* FontService::Get( uint64_t, float )
    {
        ADD_FAILURE() << "FontService::Get reached with no font service";
        return nullptr;
    }
    uint64_t FontService::DefaultFontHandle()
    {
        ADD_FAILURE() << "FontService::DefaultFontHandle reached with no font service";
        return 0;
    }
    bool FontService::RequestGlyphs( uint64_t, const std::vector<uint32_t>& )
    {
        ADD_FAILURE() << "FontService::RequestGlyphs reached with no font service";
        return false;
    }
    Icon* IconService::Get( uint64_t )
    {
        ADD_FAILURE() << "IconService::Get reached with no icon service";
        return nullptr;
    }
} // namespace Desert::Runtime

using Desert::UI::OverlayAxis;
using Desert::UI::PlaceOverlay;
using Desert::UI::Rect;
using Desert::UI::UIInput;
using Desert::UI::UIViewContext;
namespace ECS = Desert::ECS;
namespace R2D = Desert::Graphic::Render2D;
namespace DUI = Desert::UI;

namespace
{
    constexpr float kSide = 1000.0f; // Stretch at 1000x1000, so design px == screen px (scale 1)
    const Rect      kViewport{ 0.0f, 0.0f, kSide, kSide };

    // A scene with a HUD canvas at the bottom and as many overlay canvases above it as a test asks for.
    // The HUD carries one button with a listener, because "did the press reach what is underneath" is the
    // question three of these tests are really asking.
    struct World
    {
        entt::registry Registry;
        entt::entity   Hud       = entt::null;
        entt::entity   HudButton = entt::null;

        World() : Hud( MakeCanvas( 0 ) ), HudButton( MakeBox( Hud, { 0.0f, 0.0f }, { 200.0f, 100.0f } ) )
        {
            Registry.emplace<ECS::UIButtonComponent>( HudButton );
            auto& ev         = Registry.emplace<ECS::UIPointerEventsComponent>( HudButton ).Data;
            ev.OnDownMessage = "hud:down";
        }

        entt::entity MakeCanvas( int sortOrder )
        {
            const entt::entity e = Registry.create();
            auto&              c = Registry.emplace<ECS::UICanvasComponent>( e ).Data;
            c.ScaleMode          = ECS::UICanvasScaleMode::Stretch;
            c.ReferenceWidth     = kSide;
            c.ReferenceHeight    = kSide;
            c.SortOrder          = sortOrder;
            Registry.emplace<ECS::RelationshipComponent>( e );
            return e;
        }

        // An overlay canvas of @p kind named @p name, with ONE panel of @p size at the canvas origin —
        // enough content for the placement to have something to measure and move.
        entt::entity MakeOverlay( ECS::UIOverlayKind kind, const char* name, int sortOrder,
                                  glm::vec2 size = { 200.0f, 80.0f } )
        {
            const entt::entity c = MakeCanvas( sortOrder );
            auto&              o = Registry.emplace<ECS::UIOverlayComponent>( c ).Data;
            o.Kind               = kind;
            o.Name               = name;
            o.Gap                = { 10.0f, 10.0f };
            o.OpenDelay          = 0.3f;
            MakeBox( c, { 0.0f, 0.0f }, size );
            return c;
        }

        entt::entity MakeBox( entt::entity parent, glm::vec2 offMin, glm::vec2 offMax )
        {
            const entt::entity e = Registry.create();
            auto&              l = Registry.emplace<ECS::UILayoutComponent>( e ).Data;
            l.AnchorMin          = { 0.0f, 0.0f };
            l.AnchorMax          = { 0.0f, 0.0f };
            l.OffsetMin          = offMin;
            l.OffsetMax          = offMax;
            Registry.emplace<ECS::UIPanelComponent>( e );
            Registry.emplace<ECS::RelationshipComponent>( e ).Parent = parent;
            Registry.get<ECS::RelationshipComponent>( parent ).Children.push_back( e );
            return e;
        }

        ECS::UIOverlayTriggerData& Trigger( entt::entity e, const char* overlay, ECS::UIOverlayTriggerEvent on,
                                            const char* text = "" )
        {
            auto& t   = Registry.emplace<ECS::UIOverlayTriggerComponent>( e ).Data;
            t.Overlay = overlay;
            t.On      = on;
            t.Text    = text;
            return t;
        }

        // ONE FRAME of @p view over every canvas in authored order — what a host does. Returns every
        // message the frame fired, in order.
        std::vector<std::string> Frame( UIViewContext& view, const UIInput& input )
        {
            R2D::DrawList2D          dl;
            std::vector<std::string> out;
            std::string              clicked;
            DUI::BeginUIFrame( view, Registry, kViewport );
            for ( const entt::entity c : DUI::CanvasesInDrawOrder( Registry ) )
            {
                const auto drawn =
                     DUI::RenderCanvas2D( view, Registry, c, dl, /*worldViewProj=*/nullptr, &input, &clicked );
                EXPECT_TRUE( drawn.IsSuccess() ) << drawn.GetError();
            }
            DUI::EndUIFrame( view, Registry, dl, &input, /*focused=*/nullptr, &clicked, &out );
            return out;
        }
    };

    UIInput At( float x, float y )
    {
        UIInput in;
        in.MousePx = { x, y };
        return in;
    }

    // A complete click at a point: the frame that presses, then the frame that releases. Separate frames
    // because the press EDGE is derived inside the UI frame from the previous frame's button state — two
    // presses in a row with no release between them are one press, which is also true of a real mouse.
    std::vector<std::string> Click( World& w, UIViewContext& view, float x, float y, bool right = false )
    {
        UIInput down                                     = At( x, y );
        ( right ? down.MouseRightDown : down.MouseDown ) = true;
        std::vector<std::string> fired                   = w.Frame( view, down );

        UIInput up       = At( x, y );
        up.MouseReleased = !right;
        for ( std::string& m : w.Frame( view, up ) )
            fired.push_back( std::move( m ) );
        return fired;
    }

    // Push this view's wall clock @p seconds into the past, so the NEXT frame it draws measures that
    // delta. The walk reads a real clock by design; this is how a test asks it for a specific one without
    // sleeping. Capped at the walk's own 0.1 s clamp, so callers step rather than jump.
    void RewindClock( UIViewContext& view, float seconds )
    {
        view.LastFrameTime -= seconds;
    }

    // Advance @p view by @p steps frames of ~0.1 s each with the pointer parked at @p input.
    void Idle( World& w, UIViewContext& view, const UIInput& input, int steps )
    {
        for ( int i = 0; i < steps; ++i )
        {
            RewindClock( view, 0.1f );
            w.Frame( view, input );
        }
    }

    // The on-screen box an overlay canvas's content occupies, read through the LAYOUT query — i.e. the
    // answer the editor's pick and the marquee would give, with this view's own cell.
    Rect OverlayBox( World& w, UIViewContext& view, entt::entity canvas )
    {
        std::vector<DUI::UIElementNode> nodes;
        EXPECT_TRUE( DUI::EnumerateCanvas( w.Registry, canvas, kViewport, nodes, &view.CanvasState( canvas ) )
                          .IsSuccess() );
        Rect box{};
        for ( const DUI::UIElementNode& n : nodes )
            if ( n.Drawn && n.OwnRect )
                box = n.ScreenPx;
        return box;
    }
} // namespace

// ==========================================================================================================
// 1. PLACEMENT — the one mechanism, as pure math
// ==========================================================================================================

TEST( OverlayPlacement, AtTheFarEdgeTheBoxFlipsToTheOtherSideOfTheOriginInsteadOfLeavingTheView )
{
    const Rect bounds{ 0.0f, 0.0f, 1000.0f, 1000.0f };
    const Rect cursorNearRight{ 960.0f, 500.0f, 0.0f, 0.0f };

    const Rect placed =
         PlaceOverlay( { 200.0f, 80.0f }, cursorNearRight, { 10.0f, 10.0f }, bounds, OverlayAxis::Vertical );

    EXPECT_LE( placed.X + placed.W, bounds.X + bounds.W ) << "the box left the view through the right edge";
    EXPECT_GE( placed.X, bounds.X );
    EXPECT_LT( placed.X + placed.W, cursorNearRight.X + cursorNearRight.W )
         << "the box was CLAMPED against the border instead of flipping to the other side of the pointer; "
            "clamping puts it on top of what the pointer is pointing at, which is the one place it may "
            "not be";

    // THE CONTROL, and it is what separates "it flips when it must" from "it always flips". The same box
    // at the same size in the middle of the view keeps the preferred side.
    const Rect middle = PlaceOverlay( { 200.0f, 80.0f }, { 400.0f, 500.0f, 0.0f, 0.0f }, { 10.0f, 10.0f }, bounds,
                                      OverlayAxis::Vertical );
    EXPECT_FLOAT_EQ( middle.X, 410.0f );
    EXPECT_FLOAT_EQ( middle.Y, 510.0f );
}

TEST( OverlayPlacement, TheAdvanceAxisIsWhatSeparatesAMenuFromASubmenu )
{
    const Rect bounds{ 0.0f, 0.0f, 1000.0f, 1000.0f };
    const Rect item{ 100.0f, 200.0f, 240.0f, 26.0f };

    // A menu drops BELOW the point it was opened at.
    const Rect menu = PlaceOverlay( { 240.0f, 96.0f }, { 100.0f, 200.0f, 0.0f, 0.0f }, { 0.0f, 0.0f }, bounds,
                                    OverlayAxis::Vertical );
    EXPECT_FLOAT_EQ( menu.Y, 200.0f );
    EXPECT_FLOAT_EQ( menu.X, 100.0f );

    // A submenu steps BESIDE its item and keeps its top edge, which is the whole difference.
    const Rect sub = PlaceOverlay( { 240.0f, 96.0f }, item, { 0.0f, 0.0f }, bounds, OverlayAxis::Horizontal );
    EXPECT_FLOAT_EQ( sub.X, item.X + item.W );
    EXPECT_FLOAT_EQ( sub.Y, item.Y );
}

TEST( OverlayPlacement, ABoxLargerThanTheViewIsPinnedInsteadOfBeingPushedOutOfIt )
{
    const Rect bounds{ 0.0f, 0.0f, 300.0f, 300.0f };
    // Neither side of the origin has room for it, so there is no flip that helps.
    const Rect placed = PlaceOverlay( { 500.0f, 500.0f }, { 150.0f, 150.0f, 0.0f, 0.0f }, { 8.0f, 8.0f }, bounds,
                                      OverlayAxis::Vertical );
    EXPECT_FLOAT_EQ( placed.X, bounds.X );
    EXPECT_FLOAT_EQ( placed.Y, bounds.Y );
}

// ==========================================================================================================
// 2. MODALITY — a press that misses does not reach what is underneath
// ==========================================================================================================

// THE DECISIVE ONE. Both halves are here on purpose: the second is the only thing that distinguishes a
// modal from a build in which no press ever reaches anything.
TEST( OverlayModal, APressThatMissesTheDialogDoesNotReachTheElementUnderIt )
{
    World              w;
    const entt::entity modal = w.MakeOverlay( ECS::UIOverlayKind::Modal, "Confirm", 200 );
    // The dialog itself sits far from the HUD button, so the point tested is inside the HUD button and
    // outside the dialog — which is exactly "a click that missed".
    w.Registry.get<ECS::UILayoutComponent>( w.Registry.get<ECS::RelationshipComponent>( modal ).Children.front() )
         .Data.OffsetMin = { 600.0f, 600.0f };
    w.Registry.get<ECS::UILayoutComponent>( w.Registry.get<ECS::RelationshipComponent>( modal ).Children.front() )
         .Data.OffsetMax = { 800.0f, 700.0f };

    UIInput press   = At( 50.0f, 50.0f ); // inside the HUD button
    press.MouseDown = true;

    // --- Control: the modal is CLOSED, so the press is an ordinary press on the HUD.
    {
        UIViewContext                  view;
        const std::vector<std::string> fired = w.Frame( view, press );
        EXPECT_NE( std::find( fired.begin(), fired.end(), "hud:down" ), fired.end() )
             << "with no modal open the HUD button did not hear its own press — the rest of this test "
                "would then prove nothing";
        EXPECT_EQ( view.Hot, w.HudButton );
    }

    // --- The modal is open.
    {
        UIViewContext view;
        // One frame first: BeginUIFrame binds the view to this registry and drops everything it held, so a
        // cell written before the view has ever seen the scene is thrown away with it.
        w.Frame( view, At( 900.0f, 900.0f ) );
        view.CanvasState( modal ).OverlayOpen = true;
        const std::vector<std::string> fired  = w.Frame( view, press );
        EXPECT_EQ( std::find( fired.begin(), fired.end(), "hud:down" ), fired.end() )
             << "the press went straight through the modal to the button underneath";
        EXPECT_EQ( view.Hot, modal )
             << "the frame's hot element was not the modal's own scrim, which is the ONLY thing that "
                "stops the press: there is no second rule in the controls to fall back on";
    }
}

// A modal that asked for no dim is still a modal. Tying capture to the scrim's alpha would make Scrim
// Opacity two settings wearing one name — one of them undocumented.
TEST( OverlayModal, AFullyTransparentScrimStillCaptures )
{
    World              w;
    const entt::entity modal = w.MakeOverlay( ECS::UIOverlayKind::Modal, "Confirm", 200 );
    w.Registry.get<ECS::UIOverlayComponent>( modal ).Data.ScrimOpacity = 0.0f;
    // The dialog itself is far from the press point, so what is tested is the scrim and not the content.
    auto& dialog =
         w.Registry
              .get<ECS::UILayoutComponent>( w.Registry.get<ECS::RelationshipComponent>( modal ).Children.front() )
              .Data;
    dialog.OffsetMin = { 600.0f, 600.0f };
    dialog.OffsetMax = { 800.0f, 700.0f };

    UIViewContext view;
    w.Frame( view, At( 900.0f, 900.0f ) ); // bind the view to this registry first — see the test above
    view.CanvasState( modal ).OverlayOpen = true;
    UIInput press                         = At( 50.0f, 50.0f );
    press.MouseDown                       = true;

    const std::vector<std::string> fired = w.Frame( view, press );
    EXPECT_EQ( std::find( fired.begin(), fired.end(), "hud:down" ), fired.end() );
    EXPECT_EQ( view.Hot, modal );
}

TEST( OverlayModal, AClosedOverlayDrawsNothingAndIsNotTheSameStatementAsAnInvisibleCanvas )
{
    World              w;
    const entt::entity modal = w.MakeOverlay( ECS::UIOverlayKind::Modal, "Confirm", 200 );

    UIViewContext   view;
    R2D::DrawList2D dl;
    DUI::BeginUIFrame( view, w.Registry, kViewport );
    const auto closed = DUI::RenderCanvas2D( view, w.Registry, modal, dl );
    DUI::EndUIFrame( view, w.Registry, dl, /*input=*/nullptr );

    ASSERT_TRUE( closed.IsSuccess() ) << "a closed overlay is not an ERROR: the canvas was named correctly "
                                         "and simply has no pixels this frame";
    EXPECT_FALSE( closed.GetValue() );
    EXPECT_TRUE( dl.GetVertices().empty() );

    // And the layout query agrees, with a REASON rather than an empty list — "the overlay it is in is
    // closed" is a different answer from "this canvas has no children".
    std::vector<DUI::UIElementNode> nodes;
    ASSERT_TRUE(
         DUI::EnumerateCanvas( w.Registry, modal, kViewport, nodes, &view.CanvasState( modal ) ).IsSuccess() );
    ASSERT_FALSE( nodes.empty() ) << "the closed overlay reported an empty tree, which a caller cannot tell "
                                     "from a canvas with nothing in it";
    EXPECT_FALSE( nodes.front().Drawn );
    EXPECT_EQ( nodes.front().CauseBy, modal );
}

// ==========================================================================================================
// 3. TOOLTIP — the hover clock, and the edge
// ==========================================================================================================

TEST( OverlayTooltip, ItOpensOnlyAfterTheAuthoredDelayAndClosesWhenThePointerLeaves )
{
    World              w;
    const entt::entity tip = w.MakeOverlay( ECS::UIOverlayKind::Tooltip, "Tip", 400 );
    w.Trigger( w.HudButton, "Tip", ECS::UIOverlayTriggerEvent::Hover, "Explains the button" );

    UIViewContext view;
    const UIInput onButton = At( 50.0f, 50.0f );

    w.Frame( view, onButton ); // frame 1: the trigger is seen, the clock starts at zero
    EXPECT_FALSE( view.CanvasState( tip ).OverlayOpen ) << "the tooltip ignored its Open Delay";

    Idle( w, view, onButton, 2 ); // ~0.2 s — still under the 0.3 s delay
    EXPECT_FALSE( view.CanvasState( tip ).OverlayOpen );

    Idle( w, view, onButton, 2 ); // ~0.4 s
    EXPECT_TRUE( view.CanvasState( tip ).OverlayOpen ) << "the delay elapsed and nothing opened";
    EXPECT_EQ( view.OverlayTooltip, tip );

    // The trigger's Text reached the overlay through the cell's locals — never through the scene, and
    // never through the process-wide store where a second view would see it.
    EXPECT_EQ( view.CanvasState( tip ).Locals.Text( DUI::kOverlayTextKey ).value_or( "" ), "Explains the button" );
    EXPECT_FALSE( DUI::UIDataStore::Get().Has( DUI::kOverlayTextKey ) )
         << "the tooltip's text was written into the process-wide data store, where the editor's preview "
            "and the viewport would overwrite each other";

    Idle( w, view, At( 900.0f, 900.0f ), 1 );
    EXPECT_FALSE( view.CanvasState( tip ).OverlayOpen ) << "the tooltip outlived the hover that opened it";
    EXPECT_TRUE( view.OverlayTooltip == entt::null );
}

// The brief's own case, end to end through the real walk rather than through PlaceOverlay alone: the
// pointer near the right border, and the tooltip must be inside the view AND on the other side of the
// cursor.
TEST( OverlayTooltip, AtTheRightEdgeTheOpenedTooltipFlipsAndStaysInsideTheView )
{
    World              w;
    const entt::entity tip = w.MakeOverlay( ECS::UIOverlayKind::Tooltip, "Tip", 400, { 300.0f, 60.0f } );
    w.Registry.get<ECS::UIOverlayComponent>( tip ).Data.OpenDelay = 0.0f;
    // A trigger that spans the whole canvas, so the pointer can rest anywhere including at the border.
    auto& l     = w.Registry.get<ECS::UILayoutComponent>( w.HudButton ).Data;
    l.OffsetMax = { kSide, kSide };
    w.Trigger( w.HudButton, "Tip", ECS::UIOverlayTriggerEvent::Hover, "hint" );

    // --- Control first: in the middle of the view it does NOT flip.
    UIViewContext middleView;
    Idle( w, middleView, At( 400.0f, 400.0f ), 2 );
    ASSERT_TRUE( middleView.CanvasState( tip ).OverlayOpen );
    const Rect middle = OverlayBox( w, middleView, tip );
    EXPECT_GT( middle.X, 400.0f ) << "the tooltip flipped when there was room not to";

    // --- 40 px from the right border: 300 px of tooltip cannot fit to the right of the cursor.
    UIViewContext edgeView;
    Idle( w, edgeView, At( kSide - 40.0f, 400.0f ), 2 );
    ASSERT_TRUE( edgeView.CanvasState( tip ).OverlayOpen );
    const Rect edge = OverlayBox( w, edgeView, tip );

    EXPECT_LE( edge.X + edge.W, kSide ) << "the tooltip hung off the right edge of the view";
    EXPECT_GE( edge.X, 0.0f );
    EXPECT_LT( edge.X + edge.W, kSide - 40.0f )
         << "the tooltip was pushed against the border instead of flipping to the left of the cursor — it "
            "is then sitting on top of the very thing it describes";
}

// A tooltip drawn above everything must not be electable, or it takes the hover that opened it and then
// closes itself — on, off, on, off. Enforced by the walk rather than by authoring discipline.
TEST( OverlayTooltip, AnOpenTooltipNeverTakesThePointerFromWhatItDescribes )
{
    World              w;
    const entt::entity tip = w.MakeOverlay( ECS::UIOverlayKind::Tooltip, "Tip", 400, { kSide, kSide } );
    w.Registry.get<ECS::UIOverlayComponent>( tip ).Data.OpenDelay     = 0.0f;
    w.Registry.get<ECS::UIOverlayComponent>( tip ).Data.FollowPointer = false;
    w.Trigger( w.HudButton, "Tip", ECS::UIOverlayTriggerEvent::Hover, "hint" );

    UIViewContext view;
    const UIInput onButton = At( 50.0f, 50.0f );
    Idle( w, view, onButton, 3 );

    ASSERT_TRUE( view.CanvasState( tip ).OverlayOpen );
    EXPECT_EQ( view.Hot, w.HudButton )
         << "the tooltip covers the whole view and took the election from the element it is about";
}

// ==========================================================================================================
// 4. CONTEXT MENU — open on the right button, close on Escape and on a press outside
// ==========================================================================================================

TEST( OverlayContextMenu, ItOpensOnTheRightButtonAndClosesOnEscape )
{
    World              w;
    const entt::entity menu = w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Menu", 300 );
    w.Trigger( w.HudButton, "Menu", ECS::UIOverlayTriggerEvent::RightClick );

    UIViewContext view;
    Click( w, view, 50.0f, 50.0f, /*right=*/true );
    ASSERT_TRUE( view.CanvasState( menu ).OverlayOpen ) << "the right button did not open the menu";
    ASSERT_EQ( view.OverlayStack.size(), 1u );

    UIInput esc = At( 50.0f, 50.0f );
    esc.Escape  = true;
    w.Frame( view, esc );
    EXPECT_FALSE( view.CanvasState( menu ).OverlayOpen ) << "Escape did not close the context menu";
    EXPECT_TRUE( view.OverlayStack.empty() );

    // The control: an overlay that says it does not close on Escape does not.
    w.Registry.get<ECS::UIOverlayComponent>( menu ).Data.CloseOnEscape = false;
    UIViewContext second;
    Click( w, second, 50.0f, 50.0f, /*right=*/true );
    ASSERT_TRUE( second.CanvasState( menu ).OverlayOpen );
    w.Frame( second, esc );
    EXPECT_TRUE( second.CanvasState( menu ).OverlayOpen )
         << "Close On Escape is a knob that moves nothing — Escape closed the menu anyway";
}

TEST( OverlayContextMenu, APressOutsideTheMenuClosesItAndAPressInsideDoesNot )
{
    World              w;
    const entt::entity menu = w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Menu", 300 );
    w.Trigger( w.HudButton, "Menu", ECS::UIOverlayTriggerEvent::RightClick );

    UIViewContext view;
    Click( w, view, 50.0f, 50.0f, /*right=*/true );
    ASSERT_TRUE( view.CanvasState( menu ).OverlayOpen );

    // The menu was placed at the click, so a point inside its own box is a press it keeps.
    const Rect box = OverlayBox( w, view, menu );
    Click( w, view, box.X + box.W * 0.5f, box.Y + box.H * 0.5f );
    EXPECT_TRUE( view.CanvasState( menu ).OverlayOpen )
         << "clicking an item of the menu closed the menu before the item could act";

    Click( w, view, 900.0f, 900.0f );
    EXPECT_FALSE( view.CanvasState( menu ).OverlayOpen ) << "a press outside the menu left it open";
}

// NESTING IS THE SAME MECHANISM, RECURSIVELY. A submenu is a ContextMenu overlay whose trigger is an item
// of the menu below it, so it needed no machinery of its own — and this asserts the one property that
// follows from that and nothing else: closing the parent closes the child.
TEST( OverlayContextMenu, ASubmenuIsAnotherOverlayOpenedFromAnItemAndClosesWithItsParent )
{
    World              w;
    const entt::entity menu = w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Menu", 300 );
    const entt::entity sub  = w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Sub", 310 );
    w.Registry.get<ECS::UIOverlayComponent>( sub ).Data.OpenDelay = 0.0f;
    w.Trigger( w.HudButton, "Menu", ECS::UIOverlayTriggerEvent::RightClick );

    // The item of the parent menu that opens the submenu, on hover.
    const entt::entity item = w.Registry.get<ECS::RelationshipComponent>( menu ).Children.front();
    w.Trigger( item, "Sub", ECS::UIOverlayTriggerEvent::Hover );

    UIViewContext view;
    Click( w, view, 50.0f, 50.0f, /*right=*/true );
    ASSERT_TRUE( view.CanvasState( menu ).OverlayOpen );

    const Rect box = OverlayBox( w, view, menu );
    Idle( w, view, At( box.X + box.W * 0.5f, box.Y + box.H * 0.5f ), 2 );
    ASSERT_TRUE( view.CanvasState( sub ).OverlayOpen ) << "hovering the item did not open the submenu";
    EXPECT_EQ( view.OverlayStack.size(), 2u ) << "the submenu replaced its parent instead of stacking on it";

    UIInput esc = At( box.X + box.W * 0.5f, box.Y + box.H * 0.5f );
    esc.Escape  = true;
    w.Frame( view, esc );
    EXPECT_FALSE( view.CanvasState( sub ).OverlayOpen ) << "Escape closed the parent instead of the innermost";
    EXPECT_TRUE( view.CanvasState( menu ).OverlayOpen );

    Click( w, view, 900.0f, 900.0f );
    EXPECT_TRUE( view.OverlayStack.empty() );
}

// ==========================================================================================================
// 5. TOAST — it leaves on its own, it stacks, and both its stack and its queue are bounded
// ==========================================================================================================

TEST( OverlayToast, ANotificationLeavesOnItsOwnClock )
{
    World              w;
    const entt::entity toasts = w.MakeOverlay( ECS::UIOverlayKind::Toast, "Toasts", 100 );
    auto&              o      = w.Registry.get<ECS::UIOverlayComponent>( toasts ).Data;
    o.ToastLifetime           = 0.5f;
    o.ToastSlots              = 3;

    UIViewContext view;
    DUI::UIOverlayRequests::Get().Clear();
    DUI::UIOverlayRequests::Get().Raise( "Toasts", "Saved" );

    w.Frame( view, At( 900.0f, 900.0f ) );
    ASSERT_EQ( view.CanvasState( toasts ).OverlayToastVisible.size(), 1u )
         << "the raised notification never reached the view";
    EXPECT_TRUE( view.CanvasState( toasts ).OverlayOpen );
    EXPECT_EQ( view.CanvasState( toasts ).Locals.Text( DUI::OverlayToastTextKey( 0 ) ).value_or( "" ), "Saved" );
    EXPECT_EQ( view.CanvasState( toasts ).Locals.Bool( DUI::OverlayToastVisibleKey( 1 ) ).value_or( true ), false )
         << "an empty slot was left to fall back on whatever the author typed into it";

    Idle( w, view, At( 900.0f, 900.0f ), 8 ); // ~0.8 s, past its 0.5 s lifetime
    EXPECT_TRUE( view.CanvasState( toasts ).OverlayToastVisible.empty() )
         << "the notification had to be dismissed by hand";
    EXPECT_FALSE( view.CanvasState( toasts ).OverlayOpen ) << "the toast canvas stayed open with nothing to show";
}

TEST( OverlayToast, TheVisibleStackIsBoundedByTheAuthoredSlotsAndTheQueueBehindItIsBoundedToo )
{
    World              w;
    const entt::entity toasts = w.MakeOverlay( ECS::UIOverlayKind::Toast, "Toasts", 100 );
    auto&              o      = w.Registry.get<ECS::UIOverlayComponent>( toasts ).Data;
    o.ToastLifetime           = 10.0f; // long enough that nothing expires during the test
    o.ToastSlots              = 2;

    UIViewContext view;
    DUI::UIOverlayRequests::Get().Clear();
    for ( int i = 0; i < 40; ++i )
        DUI::UIOverlayRequests::Get().Raise( "Toasts", "line " + std::to_string( i ) );

    w.Frame( view, At( 900.0f, 900.0f ) );

    const auto& cell = view.CanvasState( toasts );
    EXPECT_EQ( cell.OverlayToastVisible.size(), 2u ) << "more notifications were shown than there are slots";
    EXPECT_LE( cell.OverlayToastPending.size(), 8u )
         << "the waiting queue grew without limit — forty notifications raised in one frame would be read "
            "minutes after the events they describe";

    // What survives is the NEWEST, because a notification's value is its recency: the queue drops the line
    // that has waited longest, not the one that just arrived.
    EXPECT_EQ( cell.OverlayToastPending.back(), "line 39" );
}

TEST( OverlayToast, AToastNeverTakesThePointerFromWhatIsUnderIt )
{
    World w;
    // A toast canvas covering the whole view — the worst case, and the one an author can create by
    // accident with a stack anchored to a corner and a fitter.
    const entt::entity toasts = w.MakeOverlay( ECS::UIOverlayKind::Toast, "Toasts", 100, { kSide, kSide } );
    w.Registry.get<ECS::UIOverlayComponent>( toasts ).Data.ToastLifetime = 10.0f;

    UIViewContext view;
    DUI::UIOverlayRequests::Get().Clear();
    DUI::UIOverlayRequests::Get().Raise( "Toasts", "Saved" );

    w.Frame( view, At( 50.0f, 50.0f ) ); // the notification arrives and the canvas opens this frame
    const std::vector<std::string> fired = Click( w, view, 50.0f, 50.0f );

    ASSERT_TRUE( view.CanvasState( toasts ).OverlayOpen );
    EXPECT_EQ( view.Hot, w.HudButton ) << "the toast canvas was elected over the HUD beneath it";
    EXPECT_NE( std::find( fired.begin(), fired.end(), "hud:down" ), fired.end() )
         << "the press was swallowed by a notification that is about to disappear on its own";
}

// ==========================================================================================================
// 6. THE TWO WALKS AGREE, AND THE SCENE KEEPS WHAT WAS AUTHORED
// ==========================================================================================================

// A placement applied by the renderer and not by the layout queries is the middle link that drops a
// property: the menu would draw where it was opened and be clickable where it was authored. Asserted as
// the AGREEMENT rather than on either side.
TEST( OverlayPlacement, WhereItDrawsAndWhereItCanBeClickedAreOneAnswer )
{
    World              w;
    const entt::entity menu = w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Menu", 300 );
    w.Trigger( w.HudButton, "Menu", ECS::UIOverlayTriggerEvent::RightClick );

    UIViewContext view;
    Click( w, view, 120.0f, 80.0f, /*right=*/true ); // inside the HUD button, which carries the trigger
    ASSERT_TRUE( view.CanvasState( menu ).OverlayOpen );

    const glm::vec2 shift = view.CanvasState( menu ).OverlayShift;
    ASSERT_GT( glm::length( shift ), 1.0f ) << "the menu was not displaced at all, so this proves nothing";

    // Where the LAYOUT WALK says the menu is, carrying this view's cell.
    const Rect box = OverlayBox( w, view, menu );
    EXPECT_NEAR( box.X, shift.x, 0.01f ) << "the enumeration ignored the placement the renderer applied";

    // ... and where the DRAWING WALK put it, read through the election it makes. The two are separate
    // recursions over the same tree (UICanvasLayout.hpp says so in as many words), so this is the
    // agreement and not either side of it.
    w.Frame( view, At( box.X + 4.0f, box.Y + 4.0f ) );
    EXPECT_EQ( Desert::UI::CanvasOf( w.Registry, view.Hot ), menu )
         << "the pointer landed on the pixels the enumeration says the menu occupies, and the frame elected "
            "something else — the menu draws in one place and is clickable in another";

    // The control: the position the menu was AUTHORED at is no longer where it can be clicked.
    UIViewContext third;
    Click( w, third, 120.0f, 80.0f, /*right=*/true );
    ASSERT_TRUE( third.CanvasState( menu ).OverlayOpen );
    w.Frame( third, At( 4.0f, 4.0f ) );
    EXPECT_NE( Desert::UI::CanvasOf( w.Registry, third.Hot ), menu )
         << "the menu was still elected at its unplaced authored position, so nothing was displaced";
}

// "Authored and silently not saved" is a closed defect class in this project (У13, five components). These
// two go through the SAME reflection data the scene serializer uses.
TEST( OverlayPersistence, EveryAuthoredOverlayFieldComesBackFromAReload )
{
    ECS::UIOverlayData authored;
    authored.Kind                = ECS::UIOverlayKind::Toast;
    authored.Name                = "Notifications";
    authored.Gap                 = { 7.0f, 9.0f };
    authored.OpenDelay           = 1.25f;
    authored.FollowPointer       = false;
    authored.CloseOnEscape       = false;
    authored.CloseOnClickOutside = false;
    authored.ScrimColor          = { 0.1f, 0.2f, 0.3f };
    authored.ScrimOpacity        = 0.75f;
    authored.ToastLifetime       = 6.5f;
    authored.ToastSlots          = 5;

    using namespace Desert::Reflection;
    const TypeInfo* type = ReflectionRegistry::Get().Find( "UIOverlayData" );
    ASSERT_NE( type, nullptr ) << "UIOverlayData is not reflected, so it is not serialized either";

    const rfl::Generic::Object written = SerializeReflected( *type, &authored, nullptr );

    ECS::UIOverlayData reloaded;
    DeserializeReflected( *type, &reloaded, written, nullptr );

    EXPECT_EQ( reloaded.Kind, authored.Kind );
    EXPECT_EQ( reloaded.Name, authored.Name );
    EXPECT_FLOAT_EQ( reloaded.Gap.x, authored.Gap.x );
    EXPECT_FLOAT_EQ( reloaded.Gap.y, authored.Gap.y );
    EXPECT_FLOAT_EQ( reloaded.OpenDelay, authored.OpenDelay );
    EXPECT_EQ( reloaded.FollowPointer, authored.FollowPointer );
    EXPECT_EQ( reloaded.CloseOnEscape, authored.CloseOnEscape );
    EXPECT_EQ( reloaded.CloseOnClickOutside, authored.CloseOnClickOutside );
    EXPECT_FLOAT_EQ( reloaded.ScrimOpacity, authored.ScrimOpacity );
    EXPECT_FLOAT_EQ( reloaded.ToastLifetime, authored.ToastLifetime );
    EXPECT_EQ( reloaded.ToastSlots, authored.ToastSlots );

    ECS::UIOverlayTriggerData trigger;
    trigger.Overlay = "Notifications";
    trigger.On      = ECS::UIOverlayTriggerEvent::RightClick;
    trigger.Text    = "Saved to disk";

    const TypeInfo* ttype = ReflectionRegistry::Get().Find( "UIOverlayTriggerData" );
    ASSERT_NE( ttype, nullptr ) << "UIOverlayTriggerData is not reflected, so it is not serialized either";
    const rfl::Generic::Object twritten = SerializeReflected( *ttype, &trigger, nullptr );
    ECS::UIOverlayTriggerData  treloaded;
    DeserializeReflected( *ttype, &treloaded, twritten, nullptr );
    EXPECT_EQ( treloaded.Overlay, trigger.Overlay );
    EXPECT_EQ( treloaded.On, trigger.On );
    EXPECT_EQ( treloaded.Text, trigger.Text );
}

// A name must identify ONE overlay. "There are two and I opened one of them" is the silent wrong answer
// the whole canvas-election gate next door exists to stop, one level up.
TEST( OverlayPersistence, ADuplicateOverlayNameIsRefusedByNameRatherThanResolvedToOneOfThem )
{
    World w;
    w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Menu", 300 );
    w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Menu", 310 );

    const auto found = DUI::OverlayByName( w.Registry, "Menu" );
    EXPECT_FALSE( found.IsSuccess() );
    EXPECT_NE( found.GetError().find( '2' ), std::string::npos ) << found.GetError();

    const auto missing = DUI::OverlayByName( w.Registry, "Nope" );
    EXPECT_FALSE( missing.IsSuccess() );
    EXPECT_NE( missing.GetError(), found.GetError() )
         << "'there is no such overlay' and 'there are two' came back as the same sentence";
}

// An authoring view shows every overlay as authored, so an author can see what they are building. It is
// the CELLS that are opened, which is what keeps the drawing walk and the layout walk reading one answer.
TEST( OverlayAuthoring, ADesignViewShowsEveryOverlayWhereItWasAuthored )
{
    World              w;
    const entt::entity menu = w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Menu", 300 );

    UIViewContext view;
    view.AuthoringPreview = true;
    R2D::DrawList2D dl;
    DUI::BeginUIFrame( view, w.Registry, kViewport );
    const auto drawn = DUI::RenderCanvas2D( view, w.Registry, menu, dl );
    DUI::EndUIFrame( view, w.Registry, dl, /*input=*/nullptr );

    ASSERT_TRUE( drawn.IsSuccess() ) << drawn.GetError();
    EXPECT_TRUE( drawn.GetValue() ) << "the UI Editor and Design mode cannot show the overlay being authored";
    EXPECT_FLOAT_EQ( view.CanvasState( menu ).OverlayShift.x, 0.0f );
    EXPECT_FLOAT_EQ( view.CanvasState( menu ).OverlayShift.y, 0.0f );
}

// Ю11 put materials on UI elements. An overlay's chrome is ORDINARY UI, so it gets them for nothing —
// and "for nothing" is a claim about the code having no second path, which is a thing to assert rather
// than to state. The walk resolves a material from the element's own slot through the view's backend
// (UICanvasRenderer2D::ResolveUIMaterial) and nothing on that path asks which canvas the element is in.
namespace
{
    class RecordingMaterials final : public DUI::IUIMaterialSource
    {
    public:
        const void* ResolveMaterial( const Desert::Assets::AssetHandle& handle ) override
        {
            Asked.push_back( static_cast<std::uint64_t>( handle ) );
            return &Asked; // any non-null id; the batcher only stores it
        }
        std::vector<std::uint64_t> Asked;
    };
} // namespace

TEST( OverlayAuthoring, AnElementInsideAnOverlayGetsItsMaterialLikeAnyOtherElement )
{
    World              w;
    const entt::entity menu = w.MakeOverlay( ECS::UIOverlayKind::ContextMenu, "Menu", 300 );
    const entt::entity item = w.Registry.get<ECS::RelationshipComponent>( menu ).Children.front();
    w.Registry.get<ECS::UIPanelComponent>( item ).Data.Material = Desert::Assets::AssetHandle( 0xC0FFEEull );

    RecordingMaterials materials;
    UIViewContext      view;
    view.Materials        = &materials;
    view.AuthoringPreview = true; // shown as authored, which is what an author sees while building it
    w.Frame( view, At( 900.0f, 900.0f ) );

    ASSERT_EQ( materials.Asked.size(), 1u )
         << "the overlay's own panel never reached the material path, so an overlay is NOT ordinary UI";
    EXPECT_EQ( materials.Asked.front(), 0xC0FFEEull );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
