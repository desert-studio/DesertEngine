// THE RELATION THIS GUARDS: when one edge of the editor's own window frame is dragged, the OPPOSITE edge
// must not move. Both sides of that relation are individually plausible -- a width that respects a minimum
// is correct, a position that follows the cursor is correct -- and they only disagree at the one moment the
// window reaches its minimum size, which is exactly the moment nobody tests by hand.
//
// The failure it describes is not hypothetical arithmetic: clamp the WIDTH and leave the POSITION alone,
// and a window dragged past its minimum by its left edge stops shrinking and starts WALKING to the right,
// one pixel per pixel of cursor travel, until it leaves the screen. The two numbers have to be clamped as
// one displacement or they part company.
//
// Why the arithmetic and not the gesture: the gesture cannot be synthesised on this machine (System Events
// has no assistive access), so a drag is verifiable only by a human hand. This is the whole of the decision
// the hand would be testing.

#include <Editor/Widgets/WindowResizeMath.hpp>

#include <gtest/gtest.h>

using Desert::Editor::UI::ResizeFromGrip;
using Desert::Editor::UI::WindowRect;

namespace
{
    constexpr int kMinW = 320;
    constexpr int kMinH = 240;

    // A window well clear of the minimum, at a position that is not the origin, so that a rule which
    // happens to work at (0,0) is not mistaken for a rule that works.
    constexpr WindowRect kStart = { 100, 200, 1000, 800 };

    int Right( const WindowRect& r )
    {
        return r.X + r.W;
    }
    int Bottom( const WindowRect& r )
    {
        return r.Y + r.H;
    }
} // namespace

TEST( WindowResize, AGripOfZeroMovesNothing )
{
    const WindowRect out = ResizeFromGrip( kStart, 0, 0, 400, -900, kMinW, kMinH );
    EXPECT_EQ( out.X, kStart.X );
    EXPECT_EQ( out.Y, kStart.Y );
    EXPECT_EQ( out.W, kStart.W );
    EXPECT_EQ( out.H, kStart.H );
}

TEST( WindowResize, DraggingTheRightEdgeLeavesTheLeftWhereItWas )
{
    const WindowRect out = ResizeFromGrip( kStart, +1, 0, 150, 0, kMinW, kMinH );
    EXPECT_EQ( out.X, kStart.X );
    EXPECT_EQ( out.Y, kStart.Y );
    EXPECT_EQ( out.W, kStart.W + 150 );
    EXPECT_EQ( out.H, kStart.H );
}

TEST( WindowResize, DraggingTheLeftEdgeLeavesTheRightWhereItWas )
{
    for ( const int delta : { -300, -1, 0, 1, 250, 679, 680 } )
    {
        const WindowRect out = ResizeFromGrip( kStart, -1, 0, delta, 0, kMinW, kMinH );
        EXPECT_EQ( Right( out ), Right( kStart ) ) << "left-edge drag of " << delta << " moved the right edge";
        EXPECT_EQ( out.Y, kStart.Y );
        EXPECT_EQ( out.H, kStart.H );
    }
}

TEST( WindowResize, DraggingTheTopEdgeLeavesTheBottomWhereItWas )
{
    for ( const int delta : { -300, 0, 100, 559, 560 } )
    {
        const WindowRect out = ResizeFromGrip( kStart, 0, -1, 0, delta, kMinW, kMinH );
        EXPECT_EQ( Bottom( out ), Bottom( kStart ) ) << "top-edge drag of " << delta << " moved the bottom";
        EXPECT_EQ( out.X, kStart.X );
        EXPECT_EQ( out.W, kStart.W );
    }
}

// THE ONE THIS FILE EXISTS FOR. Past the minimum the size stops changing; the relation says the far edge
// must stop moving with it, and a position clamped separately from the size does not.
TEST( WindowResize, PastTheMinimumTheFarEdgeStaysPutAndTheSizeStops )
{
    // 680 is exactly the travel that takes 1000 down to the 320 minimum; 681 and 5000 are past it.
    for ( const int delta : { 680, 681, 1200, 5000 } )
    {
        const WindowRect out = ResizeFromGrip( kStart, -1, -1, delta, delta, kMinW, kMinH );
        EXPECT_EQ( out.W, kMinW ) << "width kept shrinking past the minimum at " << delta;
        EXPECT_EQ( out.H, kMinH ) << "height kept shrinking past the minimum at " << delta;
        EXPECT_EQ( Right( out ), Right( kStart ) ) << "the window WALKED right at " << delta;
        EXPECT_EQ( Bottom( out ), Bottom( kStart ) ) << "the window WALKED down at " << delta;
    }
}

TEST( WindowResize, TheRightAndBottomEdgesRespectTheMinimumToo )
{
    const WindowRect out = ResizeFromGrip( kStart, +1, +1, -5000, -5000, kMinW, kMinH );
    EXPECT_EQ( out.X, kStart.X );
    EXPECT_EQ( out.Y, kStart.Y );
    EXPECT_EQ( out.W, kMinW );
    EXPECT_EQ( out.H, kMinH );
}

// A corner is the two edges it is made of and nothing more: the same drag applied to the two edges
// separately must land in the same place. Asserted against the composition rather than against numbers,
// because a corner that quietly grew a rule of its own is the defect this catches.
TEST( WindowResize, ACornerIsExactlyItsTwoEdges )
{
    for ( const int dx : { -400, -1, 0, 90, 900 } )
    {
        for ( const int dy : { -400, 0, 90, 900 } )
        {
            const WindowRect corner = ResizeFromGrip( kStart, -1, +1, dx, dy, kMinW, kMinH );
            const WindowRect left   = ResizeFromGrip( kStart, -1, 0, dx, dy, kMinW, kMinH );
            const WindowRect bottom = ResizeFromGrip( kStart, 0, +1, dx, dy, kMinW, kMinH );

            EXPECT_EQ( corner.X, left.X ) << "dx=" << dx << " dy=" << dy;
            EXPECT_EQ( corner.W, left.W ) << "dx=" << dx << " dy=" << dy;
            EXPECT_EQ( corner.Y, bottom.Y ) << "dx=" << dx << " dy=" << dy;
            EXPECT_EQ( corner.H, bottom.H ) << "dx=" << dx << " dy=" << dy;
        }
    }
}

// The window is never handed to the swapchain with a zero — or negative — extent in it, whatever the drag.
// A Vulkan surface refuses that extent, and the frames after it are a stream of refusals rather than a
// small window.
TEST( WindowResize, NoDragEverProducesAnExtentTheSwapchainWouldRefuse )
{
    for ( const int gx : { -1, 0, 1 } )
    {
        for ( const int gy : { -1, 0, 1 } )
        {
            for ( const int d : { -100000, -1000, -1, 0, 1, 1000, 100000 } )
            {
                const WindowRect out = ResizeFromGrip( kStart, gx, gy, d, d, kMinW, kMinH );
                EXPECT_GE( out.W, kMinW ) << "grip " << gx << "," << gy << " delta " << d;
                EXPECT_GE( out.H, kMinH ) << "grip " << gx << "," << gy << " delta " << d;
            }
        }
    }
}
