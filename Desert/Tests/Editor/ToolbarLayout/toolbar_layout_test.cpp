#include <Editor/Widgets/ToolbarLayout.hpp>

#include <gtest/gtest.h>

#include <vector>

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
    EXPECT_FLOAT_EQ( g[Slot::NextFrame].X, g[Slot::Pause].X + g[Slot::Pause].Width + Layout::kSegmentGap );
    EXPECT_FLOAT_EQ( g[Slot::Stop].X, g[Slot::NextFrame].X + g[Slot::NextFrame].Width + Layout::kSegmentGap );
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
    EXPECT_FLOAT_EQ( g[Slot::NextFrame].Width, g[Slot::Stop].Width );
}

TEST( ToolbarLayout, GroupWidthIsTheSumOfEverySlotItDraws )
{
    // Every state draws all five slots (Next Frame greyed until paused), so the width the placement
    // centres is exactly the slots plus their seams — no constant that matches no state.
    const Layout::PlaybackGroup g   = Layout::LayoutPlaybackGroup( kFrame );
    float                       sum = 0.0f;
    for ( const Layout::Span& slot : g.Slots )
        sum += slot.Width;
    EXPECT_FLOAT_EQ( g.Width, sum + Layout::kSegmentGap * static_cast<float>( g.Slots.size() - 1 ) );
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

namespace
{
    // The live 09-30 bar: Save | Undo Redo || four labelled modes || T R S Local || two snap steps. Widths
    // are the order of the 13 px UI font's measures; what matters is that the labelled row crosses the
    // middle of a 1644 pt (3288 px at 2x) bar and the icon-only row does not.
    std::vector<Layout::LeftButton> LiveLeftButtons()
    {
        const auto labelled = []( float withLabel, bool after )
        { return Layout::LeftButton{ withLabel, 30.0f, after }; };
        const auto icon  = []( bool after ) { return Layout::LeftButton{ 30.0f, 30.0f, after }; };
        const auto value = []( float w, bool after ) { return Layout::LeftButton{ w, w, after }; };
        return { labelled( 70.0f, false ),
                 icon( false ),
                 icon( false ),
                 labelled( 80.0f, true ),
                 labelled( 100.0f, false ),
                 labelled( 90.0f, false ),
                 labelled( 110.0f, false ),
                 icon( true ),
                 icon( false ),
                 icon( false ),
                 labelled( 75.0f, false ),
                 value( 80.0f, true ),
                 value( 70.0f, false ) };
    }

    // The centre group's middle for a bar [8, maxX], with the left groups laid from 8.
    float PlacedCentreMid( float maxX )
    {
        const std::vector<Layout::LeftButton> left = LiveLeftButtons();
        Layout::Row                           row{ .ContentMinX = 8.0f,
                                                   .ContentMaxX = maxX,
                                                   .LeftEnd     = 0.0f,
                                                   .CentreWidth = Layout::LayoutPlaybackGroup( kFrame ).Width,
                                                   .RightWidth  = 300.0f };
        row.LeftEnd        = 8.0f + Layout::LeftGroupsWidth( left, 2.0f, /*compact=*/false );
        const bool compact = Layout::ChooseLeftLabels( row ) == Layout::LeftLabels::IconsOnly;
        row.LeftEnd        = 8.0f + Layout::LeftGroupsWidth( left, 2.0f, compact );
        return Layout::PlaceRow( row ).CentreX + row.CentreWidth * 0.5f;
    }
} // namespace

TEST( ToolbarLayout, LeftGroupsWidthCountsSeparatorsAndSpacing )
{
    const std::vector<Layout::LeftButton> left = {
         { 50.0f, 20.0f, false }, { 40.0f, 20.0f, false }, { 30.0f, 30.0f, true } };
    EXPECT_FLOAT_EQ( Layout::LeftGroupsWidth( left, 2.0f, false ),
                     50.0f + 2.0f + 40.0f + Layout::kSeparatorAdvance + 30.0f );
    EXPECT_FLOAT_EQ( Layout::LeftGroupsWidth( left, 2.0f, true ),
                     20.0f + 2.0f + 20.0f + Layout::kSeparatorAdvance + 30.0f );
}

TEST( ToolbarLayout, PlaybackSitsOnTheMiddleOfANarrowAndAWideWindow )
{
    // The live defect: in a 3288 px window (1644 pt at 2x) the labelled left groups ended past the
    // middle and playback slid to ~61 % of the bar. Labels collapse first; the group stays on 50 %.
    for ( const float width : { 1644.0f, 3288.0f, 5120.0f } )
    {
        const float maxX = 8.0f + width;
        EXPECT_NEAR( PlacedCentreMid( maxX ), ( 8.0f + maxX ) * 0.5f, 1.0f ) << width;
    }
}

TEST( ToolbarLayout, LabelsStayWhileThereIsRoomForThem )
{
    std::vector<Layout::LeftButton> left = LiveLeftButtons();
    Layout::Row                     row{ .ContentMinX = 8.0f,
                                         .ContentMaxX = 8.0f + 3288.0f,
                                         .LeftEnd     = 0.0f,
                                         .CentreWidth = Layout::LayoutPlaybackGroup( kFrame ).Width,
                                         .RightWidth  = 300.0f };
    row.LeftEnd = 8.0f + Layout::LeftGroupsWidth( left, 2.0f, false );
    EXPECT_EQ( Layout::ChooseLeftLabels( row ), Layout::LeftLabels::Shown );
    row.ContentMaxX = 8.0f + 1644.0f;
    EXPECT_EQ( Layout::ChooseLeftLabels( row ), Layout::LeftLabels::IconsOnly );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
