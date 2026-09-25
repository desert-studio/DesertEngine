#include <Editor/Panels/FileExplorer/AssetTooltipLayout.hpp>

#include <gtest/gtest.h>

namespace Layout = Desert::Editor::AssetTooltipLayout;

namespace
{
    constexpr Layout::Rect kWindow{ 0.0f, 0.0f, 1600.0f, 900.0f };

    void ExpectInside( const Layout::Rect& r, const Layout::Rect& w )
    {
        EXPECT_GE( r.X, w.X );
        EXPECT_GE( r.Y, w.Y );
        EXPECT_LE( r.X + r.Width, w.X + w.Width );
        EXPECT_LE( r.Y + r.Height, w.Y + w.Height );
    }
} // namespace

TEST( AssetTooltipLayout, AppearsOnlyAfterTheHoverDelay )
{
    EXPECT_FALSE( Layout::ShouldShow( 0.0f ) );
    EXPECT_FALSE( Layout::ShouldShow( 0.49f ) );
    EXPECT_TRUE( Layout::ShouldShow( 0.5f ) );
}

TEST( AssetTooltipLayout, SmallContentKeepsItsSizeBelowRightOfTheCursor )
{
    const Layout::Rect r = Layout::Compute( 400.0f, 300.0f, 300.0f, 120.0f, kWindow );
    EXPECT_FLOAT_EQ( r.Width, 300.0f );
    EXPECT_FLOAT_EQ( r.Height, 120.0f );
    EXPECT_FLOAT_EQ( r.X, 400.0f + Layout::kCursorOffset );
    EXPECT_FLOAT_EQ( r.Y, 300.0f + Layout::kCursorOffset );
}

TEST( AssetTooltipLayout, HugeContentIsCappedAt420PxAndHalfTheWindow )
{
    // The old info strip's failure: content that wants the whole window gets a bounded box.
    const Layout::Rect r = Layout::Compute( 400.0f, 300.0f, 5000.0f, 5000.0f, kWindow );
    EXPECT_FLOAT_EQ( r.Width, 420.0f );
    EXPECT_FLOAT_EQ( r.Height, 450.0f );
    ExpectInside( r, kWindow );
}

TEST( AssetTooltipLayout, FlipsToTheOtherSideOfTheCursorNearTheBottomRightCorner )
{
    const Layout::Rect r = Layout::Compute( 1590.0f, 890.0f, 300.0f, 200.0f, kWindow );
    EXPECT_FLOAT_EQ( r.X, 1590.0f - Layout::kCursorOffset - 300.0f );
    EXPECT_FLOAT_EQ( r.Y, 890.0f - Layout::kCursorOffset - 200.0f );
    ExpectInside( r, kWindow );
}

TEST( AssetTooltipLayout, NeverLeavesTheWindowWhereverTheCursorIs )
{
    const Layout::Rect window{ 100.0f, 50.0f, 700.0f, 300.0f };
    for ( int ix = 0; ix <= 28; ++ix )
        for ( int iy = 0; iy <= 12; ++iy )
        {
            const float        cx = window.X + 25.0f * static_cast<float>( ix );
            const float        cy = window.Y + 25.0f * static_cast<float>( iy );
            const Layout::Rect r  = Layout::Compute( cx, cy, 380.0f, 260.0f, window );
            EXPECT_LE( r.Width, Layout::kMaxWidth );
            EXPECT_LE( r.Height, window.Height * Layout::kMaxHeightFraction );
            ExpectInside( r, window );
        }
}

TEST( AssetTooltipLayout, AWindowNarrowerThanTheCapShrinksTheTooltip )
{
    const Layout::Rect window{ 0.0f, 0.0f, 300.0f, 200.0f };
    const Layout::Rect r = Layout::Compute( 150.0f, 100.0f, 420.0f, 150.0f, window );
    EXPECT_FLOAT_EQ( r.Width, 300.0f - 2.0f * Layout::kEdgeMargin );
    EXPECT_FLOAT_EQ( r.Height, 100.0f );
    ExpectInside( r, window );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
