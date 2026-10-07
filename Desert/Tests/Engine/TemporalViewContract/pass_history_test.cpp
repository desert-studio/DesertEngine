// PASS HISTORY (TAA1 step 3): a pass-owned accumulation target (SSR, GI resolve, clouds) is reprojected through
// ViewFrame::PrevViewProjection, so it may be read only when the pass wrote it in the view's previous frame and
// the view did not reset. Replaces the per-renderer m_PrevViewProj copies (PassHistory.hpp).
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <gtest/gtest.h>

namespace PassHistoryTest
{
    using Desert::Graphic::HistoryResetReason;
    using Desert::Graphic::PassHistoryStamp;
    using Desert::Graphic::ViewFrame;

    ViewFrame Frame( const uint64_t index, const HistoryResetReason reset = HistoryResetReason::None )
    {
        ViewFrame frame;
        frame.FrameIndex   = index;
        frame.HistoryReset = reset;
        return frame;
    }
} // namespace PassHistoryTest

using namespace PassHistoryTest;

// Mutation: ReadableIn returns m_Written only -> the never-written case still passes, the others fail.
TEST( TemporalViewContract, PassHistoryIsReadableOnlyInTheFrameAfterItWasWritten )
{
    PassHistoryStamp stamp;
    EXPECT_FALSE( stamp.ReadableIn( Frame( 1 ) ) ) << "never written";

    stamp.Stamp( 4 );
    EXPECT_TRUE( stamp.ReadableIn( Frame( 5 ) ) );
    EXPECT_FALSE( stamp.ReadableIn( Frame( 6 ) ) ) << "the pass skipped frame 5: its history is two frames old";
    EXPECT_FALSE( stamp.ReadableIn( Frame( 4 ) ) ) << "frame 4 never ended (FrameFault) and is being rebuilt";
}

// Mutation: drop frame.HistoryValid() from ReadableIn -> fails for every reset reason.
TEST( TemporalViewContract, PassHistoryIsNotReadableWhenTheViewReset )
{
    for ( const HistoryResetReason reason :
          { HistoryResetReason::FirstFrame, HistoryResetReason::CameraCut, HistoryResetReason::Resize,
            HistoryResetReason::TemporalMethodChange, HistoryResetReason::PassFault } )
    {
        PassHistoryStamp stamp;
        stamp.Stamp( 9 );
        EXPECT_FALSE( stamp.ReadableIn( Frame( 10, reason ) ) ) << static_cast<int>( reason );
    }
}

// Mutation: Invalidate() does nothing -> fails.
TEST( TemporalViewContract, InvalidatedPassHistoryIsNotReadable )
{
    PassHistoryStamp stamp;
    stamp.Stamp( 2 );
    stamp.Invalidate();
    EXPECT_FALSE( stamp.ReadableIn( Frame( 3 ) ) );
}
