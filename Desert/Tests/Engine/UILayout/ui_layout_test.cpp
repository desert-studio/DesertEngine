// Unit tests for the Godot-Control-style UI layout solver (anchors + offsets + min-size, and the letterboxed
// canvas fit). Pure math — no GPU / ECS.

#include <Engine/UI/UILayout.hpp>

#include <gtest/gtest.h>

using Desert::UI::CanvasRect;
using Desert::UI::Rect;
using Desert::UI::ResolveRect;

namespace
{
    ::testing::AssertionResult RectNear( const Rect& a, const Rect& b, float eps = 1e-3f )
    {
        if ( std::abs( a.X - b.X ) > eps || std::abs( a.Y - b.Y ) > eps || std::abs( a.W - b.W ) > eps ||
             std::abs( a.H - b.H ) > eps )
            return ::testing::AssertionFailure()
                   << "(" << a.X << "," << a.Y << "," << a.W << "," << a.H << ") vs (" << b.X << "," << b.Y << ","
                   << b.W << "," << b.H << ")";
        return ::testing::AssertionSuccess();
    }

    const Rect kParent{ 0.0f, 0.0f, 1280.0f, 720.0f };
} // namespace

TEST( UILayout, FullStretchMatchesParent )
{
    const Rect r = ResolveRect( { 0, 0 }, { 1, 1 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, kParent );
    EXPECT_TRUE( RectNear( r, kParent ) );
}

TEST( UILayout, StretchWithMargins )
{
    // Full anchors, 16 px inset on every side.
    const Rect r = ResolveRect( { 0, 0 }, { 1, 1 }, { 16, 16 }, { -16, -16 }, { 0, 0 }, kParent );
    EXPECT_TRUE( RectNear( r, { 16, 16, 1248, 688 } ) );
}

TEST( UILayout, FixedTopLeft )
{
    // Zero anchors (top-left corner), positioned + sized purely by offsets.
    const Rect r = ResolveRect( { 0, 0 }, { 0, 0 }, { 10, 20 }, { 110, 70 }, { 0, 0 }, kParent );
    EXPECT_TRUE( RectNear( r, { 10, 20, 100, 50 } ) );
}

TEST( UILayout, Centered )
{
    // Anchored to the parent centre, a 200x80 box centred on it.
    const Rect r = ResolveRect( { 0.5f, 0.5f }, { 0.5f, 0.5f }, { -100, -40 }, { 100, 40 }, { 0, 0 }, kParent );
    EXPECT_TRUE( RectNear( r, { 640 - 100, 360 - 40, 200, 80 } ) );
}

TEST( UILayout, CustomMinimumSizeClamps )
{
    // A 10x10 box but with a 120x40 minimum.
    const Rect r = ResolveRect( { 0, 0 }, { 0, 0 }, { 0, 0 }, { 10, 10 }, { 120, 40 }, kParent );
    EXPECT_TRUE( RectNear( r, { 0, 0, 120, 40 } ) );
}

TEST( UILayout, CanvasFitLetterboxesWide )
{
    // 1280x720 design into a 1920x720 (too wide) viewport -> full height, centred horizontally.
    const Rect r = CanvasRect( 1280.0f, 720.0f, 1920.0f, 720.0f );
    EXPECT_TRUE( RectNear( r, { ( 1920 - 1280 ) * 0.5f, 0.0f, 1280.0f, 720.0f } ) );
}

TEST( UILayout, CanvasFitScalesDown )
{
    // 1280x720 design into a 640x360 viewport -> scaled to exactly fit (0.5x), no letterbox.
    const Rect r = CanvasRect( 1280.0f, 720.0f, 640.0f, 360.0f );
    EXPECT_TRUE( RectNear( r, { 0.0f, 0.0f, 640.0f, 360.0f } ) );
}

TEST( UILayoutGroup, HorizontalPacksLeftToRightAndStretchesHeight )
{
    using Desert::UI::LayoutGroupParams;
    using Desert::UI::LayoutGroupType;
    using Desert::UI::SolveLayoutGroup;

    LayoutGroupParams p;
    p.Type         = LayoutGroupType::Horizontal;
    p.Spacing      = 10.0f;
    p.PaddingL     = 5.0f;
    p.PaddingT     = 5.0f;
    p.PaddingR     = 5.0f;
    p.PaddingB     = 5.0f;
    p.StretchCross = true;

    const Rect                   box{ 0, 0, 500, 100 };
    const std::vector<glm::vec2> sizes = { { 40, 30 }, { 60, 30 } };
    const auto                   out   = SolveLayoutGroup( box, p, sizes );

    ASSERT_EQ( out.size(), 2u );
    EXPECT_TRUE( RectNear( out[0], { 5, 5, 40, 90 } ) );           // padded start; height stretched (100-5-5)
    EXPECT_TRUE( RectNear( out[1], { 5 + 40 + 10, 5, 60, 90 } ) ); // after first + spacing
}

TEST( UILayoutGroup, VerticalPacksTopToBottom )
{
    using namespace Desert::UI;
    LayoutGroupParams p;
    p.Type         = LayoutGroupType::Vertical;
    p.Spacing      = 8.0f;
    p.StretchCross = true;

    const Rect                   box{ 0, 0, 200, 500 };
    const std::vector<glm::vec2> sizes = { { 50, 40 }, { 50, 60 } };
    const auto                   out   = SolveLayoutGroup( box, p, sizes );

    ASSERT_EQ( out.size(), 2u );
    EXPECT_TRUE( RectNear( out[0], { 0, 0, 200, 40 } ) );  // width stretched to container
    EXPECT_TRUE( RectNear( out[1], { 0, 48, 200, 60 } ) ); // 40 + 8 spacing
}

TEST( UILayoutGroup, GridWrapsByColumns )
{
    using namespace Desert::UI;
    LayoutGroupParams p;
    p.Type     = LayoutGroupType::Grid;
    p.Spacing  = 10.0f;
    p.CellSize = { 50, 50 };
    p.Columns  = 2;

    const Rect                   box{ 0, 0, 500, 500 };
    const std::vector<glm::vec2> sizes( 3, glm::vec2( 0 ) ); // grid ignores child sizes (fixed cells)
    const auto                   out = SolveLayoutGroup( box, p, sizes );

    ASSERT_EQ( out.size(), 3u );
    EXPECT_TRUE( RectNear( out[0], { 0, 0, 50, 50 } ) );  // r0 c0
    EXPECT_TRUE( RectNear( out[1], { 60, 0, 50, 50 } ) ); // r0 c1 (50+10)
    EXPECT_TRUE( RectNear( out[2], { 0, 60, 50, 50 } ) ); // r1 c0 (wrapped)
}

// --- WHAT HAPPENS TO A UI PREFAB'S ANCHORS WHEN IT IS PUT UNDER A DIFFERENT PARENT (Ю19) -------------
//
// The question has one honest answer and this is it: NOTHING HAPPENS TO THEM. Anchors and offsets are
// AUTHORED DATA — the prefab file states them and instantiation does not rewrite them — so the instance
// resolves against whatever parent it was placed under, exactly as the same element would if an author
// dragged it there by hand.
//
// THE ALTERNATIVE WAS CONSIDERED AND REFUSED. "Preserve the pixels": rewrite the offsets at instantiation
// so the instance lands looking as it did in the prefab. That makes the prefab's authored values a
// function of where it was dropped (two sources of truth for one rect, and the file's copy the one that
// is wrong), and it DESTROYS the stretch case outright — an element anchored (0,0)-(1,1) exists in order
// to follow its parent's size, and freezing it to the pixels of the parent it was authored against is
// the opposite of what it says. The three tests below are that argument as assertions.

TEST( UILayout, InstanceKeepsAuthoredAnchorsUnderANewParent )
{
    // A fixed-size element: anchored to the parent's centre, 200x80.
    const auto resolve = []( const Rect& parent )
    { return ResolveRect( { 0.5f, 0.5f }, { 0.5f, 0.5f }, { -100, -40 }, { 100, 40 }, { 0, 0 }, parent ); };

    const Rect inSmall = resolve( { 0, 0, 400, 300 } );
    const Rect inLarge = resolve( { 0, 0, 1280, 720 } );

    // The SIZE is the authored size in both — that is what "the offsets were not rewritten" means.
    EXPECT_TRUE( RectNear( inSmall, { 100, 110, 200, 80 } ) );
    EXPECT_TRUE( RectNear( inLarge, { 540, 320, 200, 80 } ) );
}

TEST( UILayout, AStretchedInstanceFollowsTheNewParentsSize )
{
    // The case a "preserve the pixels" instantiation would silently break: a full-bleed background.
    const auto resolve = []( const Rect& parent )
    { return ResolveRect( { 0, 0 }, { 1, 1 }, { 16, 16 }, { -16, -16 }, { 0, 0 }, parent ); };

    EXPECT_TRUE( RectNear( resolve( { 0, 0, 400, 300 } ), { 16, 16, 368, 268 } ) );
    EXPECT_TRUE( RectNear( resolve( { 0, 0, 1280, 720 } ), { 16, 16, 1248, 688 } ) );
}

TEST( UILayout, TwoInstancesUnderIdenticalParentsResolveIdentically )
{
    // The arithmetic half of the frame claim "a prefab inserted twice looks the same": identical
    // authored values against identical parent rects give the same rect to the last bit. If this could
    // fail, no pixel comparison of two instances would mean anything.
    const Rect parentA{ 0, 0, 640, 480 };
    const Rect parentB{ 0, 0, 640, 480 };

    const Rect a = ResolveRect( { 0, 0 }, { 0, 0 }, { 12, 34 }, { 212, 114 }, { 0, 0 }, parentA );
    const Rect b = ResolveRect( { 0, 0 }, { 0, 0 }, { 12, 34 }, { 212, 114 }, { 0, 0 }, parentB );

    EXPECT_EQ( a.X, b.X );
    EXPECT_EQ( a.Y, b.Y );
    EXPECT_EQ( a.W, b.W );
    EXPECT_EQ( a.H, b.H );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
