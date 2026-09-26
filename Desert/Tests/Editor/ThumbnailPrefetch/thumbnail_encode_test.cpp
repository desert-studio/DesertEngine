// "A capture's CPU half runs without a device, and captures are paced by main-thread milliseconds."
//
// TH3: a thumbnail capture cost ~215 ms on the main thread (readback 80, downscale 23, PNG ~108). The
// downscale and the PNG now run on a worker through ThumbnailEncode, and ThumbnailService dispatches a new
// capture only while CaptureBudget owes nothing. Both are plain logic and are driven here without Vulkan.
#include <Editor/Widgets/ThumbnailEncode.hpp>

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    using Desert::Editor::ThumbnailEncode::CaptureBudget;
    namespace Encode = Desert::Editor::ThumbnailEncode;

    // A capture's measured main-thread cost before TH3, per frame it ran in.
    constexpr double kOldCaptureMs = 215.0;
} // namespace

TEST( ThumbnailEncode, DownscaleAveragesEachBlock )
{
    // 4x4 -> 2x2: each output pixel is the mean of one 2x2 block.
    std::vector<uint8_t> src( 4 * 4 * 4, 0 );
    for ( uint32_t y = 0; y < 4; ++y )
        for ( uint32_t x = 0; x < 4; ++x )
            src[( ( y * 4 ) + x ) * 4] = static_cast<uint8_t>( ( x < 2 && y < 2 ) ? ( ( x + y ) * 40 ) : 200 );
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
    EXPECT_FALSE( Encode::Downscale( std::vector<uint8_t>( 3 * 3 * 4 ), 3, 2 ).IsSuccess() );
}

TEST( ThumbnailEncode, WritePngLeavesTheFinishedPictureAndNoPart )
{
    const auto dir = std::filesystem::temp_directory_path() / "th3_encode_test";
    std::filesystem::remove_all( dir );
    const std::string png = ( dir / "sub" / "a.png" ).string();

    std::vector<uint8_t> rgba( 8 * 8 * 4, 0 );
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

    int      w = 0;
    int      h = 0;
    int      n = 0;
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

TEST( ThumbnailEncode, SixteenCapturesCannotStackIntoConsecutiveFrames )
{
    // Sixteen materials queued at once, each capture charging its old main-thread cost: the budget spreads
    // the dispatches so the average main-thread cost per frame stays within one constant.
    CaptureBudget budget;
    int           dispatched = 0;
    int           frames     = 0;
    double        spent      = 0.0;
    while ( dispatched < 16 )
    {
        budget.EndFrame();
        ++frames;
        if ( budget.MayDispatch() )
        {
            budget.Spend( kOldCaptureMs );
            spent += kOldCaptureMs;
            ++dispatched;
        }
    }
    EXPECT_LE( spent / frames, CaptureBudget::kMainThreadMsPerFrame + ( kOldCaptureMs / frames ) );
    EXPECT_GE( frames, static_cast<int>( 15 * kOldCaptureMs / CaptureBudget::kMainThreadMsPerFrame ) );
}
