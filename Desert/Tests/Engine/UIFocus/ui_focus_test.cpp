// UIFocus: where keyboard / gamepad navigation sends focus (Engine/UI/UIFocus.hpp).
//
// Invariants only: the spatial pick (the nearest control in the band, never one outside it), the edge
// rules, and the Tab / Shift+Tab round trip. Each test fails if FindNextFocusable degrades back to the
// draw-order step it replaced.

#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIFocus.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace UI = Desert::UI;
using UI::FocusEntry;
using UI::NodeId;
using UI::Rect;
using UI::UINavigation;
using UI::UINavigationRule;

namespace
{
    const Rect kView{ 0.0f, 0.0f, 1000.0f, 1000.0f };

    NodeId N( std::uint32_t i )
    {
        return static_cast<NodeId>( i );
    }

    // Draw order deliberately NOT spatial order: TL, BR, TR, BL. A linear step would give the wrong answer
    // for every spatial request below.
    //   TL (100,100)-(200,150)   TR (400,100)-(500,150)
    //   BL (100,400)-(200,450)   BR (400,400)-(500,450)
    std::vector<FocusEntry> Grid()
    {
        return { { N( 1 ), { 100, 100, 100, 50 } },
                 { N( 4 ), { 400, 400, 100, 50 } },
                 { N( 2 ), { 400, 100, 100, 50 } },
                 { N( 3 ), { 100, 400, 100, 50 } } };
    }
} // namespace

TEST( UIFocus, SpatialPicksTheNeighbourInEachDirection )
{
    const auto g = Grid();
    EXPECT_EQ( UI::FindNextFocusable( g, N( 1 ), UINavigation::Right, kView ), N( 2 ) );
    EXPECT_EQ( UI::FindNextFocusable( g, N( 1 ), UINavigation::Down, kView ), N( 3 ) );
    EXPECT_EQ( UI::FindNextFocusable( g, N( 4 ), UINavigation::Left, kView ), N( 3 ) );
    EXPECT_EQ( UI::FindNextFocusable( g, N( 4 ), UINavigation::Up, kView ), N( 2 ) );
}

TEST( UIFocus, SpatialPrefersTheNearestEdgeInTheBand )
{
    std::vector<FocusEntry> const row = {
         { N( 1 ), { 0, 0, 50, 50 } }, { N( 2 ), { 400, 0, 50, 50 } }, { N( 3 ), { 100, 10, 50, 50 } } };
    EXPECT_EQ( UI::FindNextFocusable( row, N( 1 ), UINavigation::Right, kView ), N( 3 ) );
}

TEST( UIFocus, SpatialNeverTakesAControlOutsideTheBand )
{
    // Right of the source but entirely below its band: Escape at the edge leaves focus where it is.
    std::vector<FocusEntry> const e = { { N( 1 ), { 0, 0, 50, 50 } }, { N( 2 ), { 200, 300, 50, 50 } } };
    EXPECT_EQ( UI::FindNextFocusable( e, N( 1 ), UINavigation::Right, kView ), NodeId::Null );
}

TEST( UIFocus, EdgeRules )
{
    const auto g = Grid();
    EXPECT_EQ( UI::FindNextFocusable( g, N( 2 ), UINavigation::Right, kView, UINavigationRule::Stop ),
               NodeId::Null );
    EXPECT_EQ( UI::FindNextFocusable( g, N( 2 ), UINavigation::Right, kView, UINavigationRule::Wrap ), N( 1 ) );
    EXPECT_EQ( UI::FindNextFocusable( g, N( 3 ), UINavigation::Down, kView, UINavigationRule::Wrap ), N( 1 ) );
}

TEST( UIFocus, TabThenShiftTabIsARoundTrip )
{
    const auto g = Grid();
    for ( const FocusEntry& e : g )
    {
        const NodeId next = UI::FindNextFocusable( g, e.Node, UINavigation::Next, kView );
        EXPECT_NE( next, e.Node );
        EXPECT_EQ( UI::FindNextFocusable( g, next, UINavigation::Previous, kView ), e.Node );
    }
    EXPECT_EQ( UI::FindNextFocusable( g, N( 3 ), UINavigation::Next, kView ), N( 1 ) ); // wraps
}

TEST( UIFocus, NothingFocusedLandsOnTheFirst )
{
    EXPECT_EQ( UI::FindNextFocusable( Grid(), NodeId::Null, UINavigation::Down, kView ), N( 1 ) );
}

TEST( UIFocus, ShiftTabIsPreviousAndPlainTabIsNext )
{
    UI::UIInput in;
    in.Keys.push_back( { Common::KeyCode::Tab, UI::UIKeyMods::Shift, false } );
    EXPECT_EQ( UI::NavigationOf( in ), UINavigation::Previous );
    in.Keys.push_back( { Common::KeyCode::Tab, UI::UIKeyMods::None, false } );
    EXPECT_EQ( UI::NavigationOf( in ), UINavigation::Next );
}

// A focused text field: W and S are letters, not Up / Down; the arrows still navigate.
TEST( UIFocus, TextEntryKeepsWAndS )
{
    UI::UIInput in;
    in.Keys.push_back( { Common::KeyCode::W, UI::UIKeyMods::None, false } );
    EXPECT_EQ( UI::NavigationOf( in ), UINavigation::Up );
    EXPECT_EQ( UI::NavigationOf( in, /*textEntry=*/true ), UINavigation::None );
    in.Keys.push_back( { Common::KeyCode::Down, UI::UIKeyMods::None, false } );
    EXPECT_EQ( UI::NavigationOf( in, /*textEntry=*/true ), UINavigation::Down );
}

// A key a control consumed (a focused slider's Left) is not a navigation; the rest of the frame's keys are.
TEST( UIFocus, ConsumedKeysDoNotNavigate )
{
    UI::UIInput in;
    in.Keys.push_back( { Common::KeyCode::Up, UI::UIKeyMods::None, false } );
    in.Keys.push_back( { Common::KeyCode::Left, UI::UIKeyMods::None, false } );
    const std::vector<Common::KeyCode> consumed = { Common::KeyCode::Left };
    EXPECT_EQ( UI::NavigationOf( in, false, consumed ), UINavigation::Up );
    EXPECT_EQ( UI::NavigationOf( in ), UINavigation::Left );
}

// Wrap inside a narrower boundary (a container's UINavigationData box) restarts inside THAT box, so a
// control outside it in the same band is never taken.
TEST( UIFocus, WrapStaysInsideTheContainerBox )
{
    const std::vector<FocusEntry> e = {
         { N( 1 ), { 0, 0, 50, 50 } }, { N( 2 ), { 100, 0, 50, 50 } }, { N( 3 ), { 300, 0, 50, 50 } } };
    const UI::Rect box{ 0, 0, 160, 60 };
    EXPECT_EQ( UI::FindNextFocusable( e, N( 2 ), UINavigation::Right, box, UINavigationRule::Wrap ), N( 1 ) );
    EXPECT_EQ( UI::FindNextFocusable( e, N( 2 ), UINavigation::Right, kView, UINavigationRule::Escape ), N( 3 ) );
}
