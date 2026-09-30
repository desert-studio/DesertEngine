#include <Editor/Widgets/ToolbarLayout.hpp>

#include <gtest/gtest.h>

namespace Layout = Desert::Editor::ToolbarLayout;
using Slot       = Layout::PlaybackSlot;

namespace
{
    constexpr float kFrame = 26.0f;

    Layout::Row WideRow( float leftEnd )
    {
        return Layout::Row{ .ContentMinX = 8.0f,
                            .ContentMaxX = 1912.0f,
                            .LeftEnd     = leftEnd,
                            .CentreWidth = Layout::LayoutPlaybackGroup( kFrame ).Width,
                            .RightWidth  = 300.0f };
    }
} // namespace

TEST( ToolbarLayout, PlaybackSlotsAreOneContiguousGroupInUeOrder )
{
    const Layout::PlaybackGroup g = Layout::LayoutPlaybackGroup( kFrame, 100.0f );
    EXPECT_FLOAT_EQ( g[Slot::Play].X, 100.0f );
    EXPECT_FLOAT_EQ( g[Slot::Options].X, g[Slot::Play].X + g[Slot::Play].Width + Layout::kSegmentGap );
    EXPECT_FLOAT_EQ( g[Slot::Pause].X, g[Slot::Options].X + g[Slot::Options].Width + Layout::kSegmentGap );
    EXPECT_FLOAT_EQ( g[Slot::Stop].X, g[Slot::Pause].X + g[Slot::Pause].Width + Layout::kSegmentGap );
    EXPECT_FLOAT_EQ( g.Width, g[Slot::Stop].X + g[Slot::Stop].Width - 100.0f );
}

TEST( ToolbarLayout, OptionsArrowIsWideEnoughForItsGlyphAndPauseMatchesStop )
{
    // The defect: the ▾ was 0.6 frame wide, narrower than its own 8 px padding each side, and drew as
    // a clipped sliver. The glyph is about half a frame wide; the slot has to hold it with air.
    const Layout::PlaybackGroup g = Layout::LayoutPlaybackGroup( kFrame );
    EXPECT_GE( g[Slot::Options].Width, kFrame * 0.8f );
    EXPECT_GT( g[Slot::Play].Width, g[Slot::Pause].Width );
    EXPECT_FLOAT_EQ( g[Slot::Pause].Width, g[Slot::Stop].Width );
}

TEST( ToolbarLayout, GroupWidthDependsOnFrameHeightOnly )
{
    // No scene-state parameter exists: Edit, Play and Paused draw the same four slots.
    EXPECT_FLOAT_EQ( Layout::LayoutPlaybackGroup( kFrame, 0.0f ).Width,
                     Layout::LayoutPlaybackGroup( kFrame, 777.0f ).Width );
    EXPECT_GT( Layout::LayoutPlaybackGroup( 2.0f * kFrame ).Width, Layout::LayoutPlaybackGroup( kFrame ).Width );
}

TEST( ToolbarLayout, CentreGroupSitsOnTheBarMiddleWhateverTheLeftGroupsRead )
{
    // The snap labels ("5 m" / "50 cm") and World/Local change the left groups' width every few
    // seconds of editing; with room to spare the playback group must not move with them.
    const Layout::RowPlacement a = Layout::PlaceRow( WideRow( 520.0f ) );
    const Layout::RowPlacement b = Layout::PlaceRow( WideRow( 610.0f ) );
    const float                w = WideRow( 0.0f ).CentreWidth;
    EXPECT_FLOAT_EQ( a.CentreX, b.CentreX );
    EXPECT_FLOAT_EQ( a.CentreX + w * 0.5f, ( 8.0f + 1912.0f ) * 0.5f );
}

TEST( ToolbarLayout, RightGroupIsFlushRightWhenThereIsRoom )
{
    const Layout::RowPlacement p = Layout::PlaceRow( WideRow( 520.0f ) );
    EXPECT_FLOAT_EQ( p.RightX, 1912.0f - 300.0f );
}

TEST( ToolbarLayout, NarrowBarSlidesTheCentreButNeverOverlapsANeighbour )
{
    for ( float maxX = 400.0f; maxX <= 2000.0f; maxX += 7.0f )
    {
        Layout::Row row   = WideRow( 520.0f );
        row.ContentMaxX   = maxX;
        const auto placed = Layout::PlaceRow( row );
        EXPECT_GE( placed.CentreX, row.LeftEnd + Layout::kGroupGap ) << maxX;
        EXPECT_GE( placed.RightX, placed.CentreX + row.CentreWidth + Layout::kGroupGap ) << maxX;
        EXPECT_LE( placed.RightX + row.RightWidth,
                   std::max( maxX, placed.CentreX + row.CentreWidth + Layout::kGroupGap + row.RightWidth ) )
             << maxX;
    }
}

TEST( ToolbarLayout, CentreSlidesLeftOfMiddleBeforeItPushesTheRightGroupOff )
{
    // Room for all three groups, but not with playback on the exact middle: it gives way to the right
    // group rather than the right group leaving the bar.
    Layout::Row row = WideRow( 100.0f );
    row.ContentMaxX = 8.0f + 700.0f;
    row.RightWidth  = 300.0f;
    const auto p    = Layout::PlaceRow( row );
    EXPECT_FLOAT_EQ( p.RightX, row.ContentMaxX - row.RightWidth );
    EXPECT_FLOAT_EQ( p.CentreX + row.CentreWidth + Layout::kGroupGap, p.RightX );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
