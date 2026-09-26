// "A capture's CPU half runs without a device, and captures are paced by main-thread milliseconds."
//
// TH3: a thumbnail capture cost ~215 ms on the main thread (readback 80, downscale 23, PNG ~108). The
// downscale and the PNG now run on a worker through ThumbnailEncode, and ThumbnailService dispatches a new
// capture once what CaptureBudget is owed fits inside one frame. Both are plain logic and are driven here
// without Vulkan.
#include <Editor/Widgets/ThumbnailEncode.hpp>

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    using Desert::Editor::ThumbnailEncode::CaptureBudget;
    namespace Encode = Desert::Editor::ThumbnailEncode;

    // Measured on the main thread (TH3, 15 Materials, editor-d1.log): the idle Tick between captures, the
    // first render Tick of a capture, a later in-flight Tick, and the first capture of a session, which
    // compiles its pipelines.
    constexpr double kIdleTickMs         = 0.00004;
    constexpr double kRenderTickMs       = 1.1;
    constexpr double kInFlightTickMs     = 0.2;
    constexpr double kFirstCaptureTickMs = 333.0;
    constexpr int    kRenderTicks        = 5;
    constexpr int    kInFlightTicks      = 21;

    // ThumbnailService::TickCapture's order on one frame: repay, charge the renderer's Tick, then dispatch
    // only with nothing in flight and the budget allowing it. Returns the frames the queue took to drain and
    // the longest run of frames with a request queued and nothing in flight.
    struct Drain
    {
        int Frames      = 0;
        int LongestIdle = 0;
    };
    Drain DrainQueue( int captures )
    {
        CaptureBudget budget;
        Drain         out;
        int           queued   = captures;
        int           inFlight = 0; // ticks left of the capture in flight
        int           tick     = 0; // ticks the capture in flight has had
        int           idle     = 0;
        bool          first    = true;
        while ( ( queued > 0 || inFlight > 0 ) && out.Frames < 100000 )
        {
            ++out.Frames;
            budget.EndFrame();
            if ( inFlight > 0 )
            {
                const double renderOrFlight = tick < kRenderTicks ? kRenderTickMs : kInFlightTickMs;
                budget.Spend( first && tick == 0 ? kFirstCaptureTickMs : renderOrFlight );
                ++tick;
                if ( --inFlight == 0 )
                    first = false;
                continue; // the frame that settles a capture does not dispatch the next
            }
            budget.Spend( kIdleTickMs );
            if ( !budget.MayDispatch() )
            {
                out.LongestIdle = std::max( out.LongestIdle, ++idle );
                continue;
            }
            idle = 0;
            --queued;
            inFlight = kRenderTicks + kInFlightTicks;
            tick     = 0;
        }
        return out;
    }
} // namespace

TEST( ThumbnailEncode, DownscaleAveragesEachBlock )
{
    // 4x4 -> 2x2: each output pixel is the mean of one 2x2 block.
    std::vector<uint8_t> src( size_t{ 4 } * 4 * 4, 0 );
    for ( uint32_t y = 0; y < 4; ++y )
        for ( uint32_t x = 0; x < 4; ++x )
            src[( ( static_cast<size_t>( y ) * 4 ) + x ) * 4] =
                 static_cast<uint8_t>( ( x < 2 && y < 2 ) ? ( ( x + y ) * 40 ) : 200 );
    const auto out = Encode::Downscale( src, 4, 2 );
    ASSERT_TRUE( out.IsSuccess() ) << out.GetError();
    EXPECT_EQ( out.GetValue()[0], ( 0 + 40 + 40 + 80 ) / 4 );
    EXPECT_EQ( out.GetValue()[4], 200 );
}

TEST( ThumbnailEncode, DownscaleRefusesAMismatchedBufferByName )
{
    const auto out = Encode::Downscale( std::vector<uint8_t>( 10 ), 4, 2 );
    ASSERT_FALSE( out.IsSuccess() );
    EXPECT_NE( out.GetError().find( "expected 64" ), std::string::npos ) << out.GetError();
    EXPECT_FALSE( Encode::Downscale( std::vector<uint8_t>( size_t{ 3 } * 3 * 4 ), 3, 2 ).IsSuccess() );
}

TEST( ThumbnailEncode, WritePngLeavesTheFinishedPictureAndNoPart )
{
    const auto dir = std::filesystem::temp_directory_path() / "th3_encode_test";
    std::filesystem::remove_all( dir );
    const std::string png = ( dir / "sub" / "a.png" ).string();

    std::vector<uint8_t> rgba( size_t{ 8 } * 8 * 4, 0 );
    for ( std::size_t i = 0; i < rgba.size(); i += 4 )
    {
        rgba[i]     = 10;
        rgba[i + 1] = 20;
        rgba[i + 2] = 30;
        rgba[i + 3] = 255;
    }
    const auto written = Encode::WritePng( rgba, 8, png );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    EXPECT_FALSE( std::filesystem::exists( png + ".part" ) );

    int      w    = 0;
    int      h    = 0;
    int      n    = 0;
    uint8_t* back = stbi_load( png.c_str(), &w, &h, &n, 4 );
    ASSERT_NE( back, nullptr );
    EXPECT_EQ( w, 8 );
    EXPECT_EQ( h, 8 );
    EXPECT_EQ( back[0], 10 );
    EXPECT_EQ( back[2], 30 );
    stbi_image_free( back );
    std::filesystem::remove_all( dir );
}

TEST( ThumbnailEncode, ABudgetInDebtRefusesTheNextCaptureUntilRepaid )
{
    CaptureBudget budget;
    EXPECT_TRUE( budget.MayDispatch() );
    budget.Spend( 3.0 * CaptureBudget::kMainThreadMsPerFrame );
    EXPECT_FALSE( budget.MayDispatch() );
    budget.EndFrame();
    budget.EndFrame();
    EXPECT_FALSE( budget.MayDispatch() );
    budget.EndFrame();
    EXPECT_TRUE( budget.MayDispatch() );
    budget.EndFrame(); // repayment never goes into credit
    EXPECT_DOUBLE_EQ( budget.OwedMs(), 0.0 );
}

TEST( ThumbnailEncode, AnIdleTickCostingNanosecondsDoesNotHoldTheNextCapture )
{
    // The defect TH3 measured: repaid to zero, then the idle Tick charged 40 ns, and "owes nothing" was
    // false until a Tick measured exactly 0 ns — the queue sat idle 0.1-2.8 s between 0.26 s captures.
    CaptureBudget budget;
    budget.Spend( kRenderTickMs );
    for ( int frame = 0; frame < 3; ++frame )
    {
        budget.EndFrame();
        budget.Spend( kIdleTickMs );
    }
    EXPECT_TRUE( budget.MayDispatch() ) << "owed " << budget.OwedMs() << " ms";
}

TEST( ThumbnailEncode, AOneOffSpikeHoldsTheQueueForAtMostMaxWaitFrames )
{
    // The first capture compiles pipelines: 333 ms in one Tick. Repaying it at 2 ms a frame held the queue
    // for 166 frames (2 s measured); the cap bounds the wait whatever the spike.
    CaptureBudget budget;
    budget.Spend( kFirstCaptureTickMs );
    int waited = 0;
    while ( waited <= CaptureBudget::kMaxWaitFrames )
    {
        budget.EndFrame();
        budget.Spend( kIdleTickMs );
        ++waited;
        if ( budget.MayDispatch() )
            break;
    }
    EXPECT_TRUE( budget.MayDispatch() );
    EXPECT_LE( waited, CaptureBudget::kMaxWaitFrames );
}

TEST( ThumbnailEncode, FifteenQueuedCapturesDrainWithOneAlwaysInFlightWithinTheWaitBound )
{
    // The Materials folder of the TH3 measurement, in TickCapture's own order. Progress: with requests
    // queued, the queue is never without a capture in flight for more than kMaxWaitFrames frames, so the
    // whole queue drains in 15 captures' own frames plus that bound each.
    const Drain drain = DrainQueue( 15 );
    EXPECT_LE( drain.LongestIdle, CaptureBudget::kMaxWaitFrames );
    EXPECT_LE( drain.Frames, 15 * ( kRenderTicks + kInFlightTicks + CaptureBudget::kMaxWaitFrames ) );
    // One capture at a time: the queue cannot drain faster than its captures' own frames.
    EXPECT_GE( drain.Frames, 15 * ( kRenderTicks + kInFlightTicks ) );
}
