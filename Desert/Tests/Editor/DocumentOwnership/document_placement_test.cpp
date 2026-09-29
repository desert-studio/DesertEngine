// WHERE A DOCUMENT OPENS: in the level viewport's dock node, as a tab beside it — the main work area, the way
// Unreal opens an asset editor — and, when there is no such node to join, floating centred at 70 % of the
// editor. The rule lives in Editor/Core/DocumentPlacement.hpp because EditorLayer.cpp, which applies it, is
// compiled by no suite.

#include <Editor/Core/DocumentPlacement.hpp>

#include <gtest/gtest.h>

#include <limits>

namespace
{
    using Desert::Editor::DocumentPlacement::kFloatingShare;
    using Desert::Editor::DocumentPlacement::Place;
    using Desert::Editor::DocumentPlacement::Placement;

    // The frame the owner judges: a 1600 x 900 editor whose work area starts under the menu bar.
    const glm::vec2 kWorkPos( 0.0f, 20.0f );
    const glm::vec2 kWorkSize( 1600.0f, 880.0f );
} // namespace

TEST( DocumentPlacement, JoinsTheLevelViewportsNodeAsATab )
{
    const Placement p = Place( 0x7u, /*nodeLive=*/true, kWorkPos, kWorkSize );
    EXPECT_TRUE( p.Docked() );
    EXPECT_EQ( p.DockId, 0x7u ) << "the document must share the level viewport's node, not a side column";
}

TEST( DocumentPlacement, FloatsLargeAndCentredWhenTheViewportFloats )
{
    const Placement p = Place( 0u, /*nodeLive=*/false, kWorkPos, kWorkSize );
    ASSERT_FALSE( p.Docked() );
    EXPECT_FLOAT_EQ( p.Size.x, kWorkSize.x * kFloatingShare );
    EXPECT_FLOAT_EQ( p.Size.y, kWorkSize.y * kFloatingShare );
    // Centred: the window's centre is the work area's centre.
    const glm::vec2 centre     = p.Pos + p.Size * 0.5f;
    const glm::vec2 workCentre = kWorkPos + kWorkSize * 0.5f;
    EXPECT_FLOAT_EQ( centre.x, workCentre.x );
    EXPECT_FLOAT_EQ( centre.y, workCentre.y );
}

TEST( DocumentPlacement, ADeadNodeIsNotJoined )
{
    // A window keeps the id of a node a later layout dropped; docking into it would make ImGui build a
    // free-floating node of its own — the small window this rule replaces.
    const Placement p = Place( 0x42u, /*nodeLive=*/false, kWorkPos, kWorkSize );
    EXPECT_FALSE( p.Docked() );
    EXPECT_GT( p.Size.x, 1000.0f ) << "a document with nowhere to dock must still open large";
}

TEST( DocumentPlacement, AnUnmeasurableWorkAreaGivesNoNegativeOrInfiniteWindow )
{
    const float     nan = std::numeric_limits<float>::quiet_NaN();
    const Placement p   = Place( 0u, false, glm::vec2( nan, nan ), glm::vec2( -5.0f, nan ) );
    EXPECT_FALSE( p.Docked() );
    EXPECT_EQ( p.Size, glm::vec2( 0.0f ) );
    EXPECT_EQ( p.Pos, glm::vec2( 0.0f ) );
}

// A LEVEL TIMELINE (the Sequencer) opens in the drawer under the viewport, never as a tab that covers it.
TEST( DocumentPlacement, ATimelineOpensInTheDrawerUnderTheViewportNotBesideIt )
{
    using Desert::Editor::DocumentPlacement::Remembered;
    using Desert::Editor::DocumentPlacement::ResolveTimeline;
    constexpr uint32_t kScene  = 0x10u;
    constexpr uint32_t kDrawer = 0x20u;

    const auto fresh =
         ResolveTimeline( "Sequencer", nullptr, kDrawer, true, kScene, true, false, kWorkPos, kWorkSize );
    EXPECT_EQ( fresh.Place.DockId, kDrawer ) << "first opening: the drawer";
    EXPECT_TRUE( fresh.Report.empty() );

    const Remembered beside{};
    EXPECT_EQ( ResolveTimeline( "Sequencer", &beside, kDrawer, true, kScene, true, false, kWorkPos, kWorkSize )
                    .Place.DockId,
               kDrawer )
         << "'beside the scene' is exactly what hides the viewport, remembered or not";

    const Remembered mine{ Remembered::Where::DockNode, 0x30u, glm::vec2( 0.0f ), glm::vec2( 0.0f ) };
    EXPECT_EQ( ResolveTimeline( "Sequencer", &mine, kDrawer, true, kScene, true, true, kWorkPos, kWorkSize )
                    .Place.DockId,
               0x30u )
         << "a node the person chose stands";

    EXPECT_EQ( ResolveTimeline( "Sequencer", nullptr, kDrawer, false, kScene, true, false, kWorkPos, kWorkSize )
                    .Place.DockId,
               kScene )
         << "no live drawer: the ordinary rule";
    EXPECT_EQ( ResolveTimeline( "Sequencer", nullptr, kScene, true, kScene, true, false, kWorkPos, kWorkSize )
                    .Place.DockId,
               kScene )
         << "a drawer that IS the scene's node is no drawer";
}
