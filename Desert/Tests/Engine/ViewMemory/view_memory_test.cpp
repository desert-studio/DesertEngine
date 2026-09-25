// "WHAT DOES ONE VIEW COST, BY TARGET" — the census in Graphic/ViewMemory.hpp, pinned.
//
// WHY. The log measured ~245 bytes a pixel for the main viewport (UE's deferred path: 60-100) plus
// 320 MiB of cascades whatever the view size, and nothing could say which targets made the number. The
// table below is printed on every run; the totals are pinned so that the format and sharing work that
// follows (RT1 steps 3-5) shows up here as a moved number, not as a vague "memory went down".
//
// WHAT WOULD MAKE THIS RED: a target added to or dropped from a view without the census being told; a
// format change (the pinned bytes/pixel moves); the preview profile quietly regaining clouds, SSR, GI or
// the full shadow budget (the main-minus-preview difference shrinks).

#include <Engine/Graphic/ViewMemory.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>

namespace
{
    using namespace Desert::Graphic;

    constexpr uint32_t kW = 1920;
    constexpr uint32_t kH = 1080;

    std::vector<std::string_view> RowNames( const ViewProfile& profile )
    {
        std::vector<std::string_view> out;
        for ( const auto& row : ViewTargetCensus( profile, kW, kH ) )
            out.push_back( row.Name );
        return out;
    }
} // namespace

TEST( ViewMemory, PrintsTheCensusForMainAndPreview )
{
    std::cout << "[main]    " << FormatViewTargetCensus( ViewTargetCensus( kSceneViewProfile, kW, kH ), kW, kH );
    std::cout << "[preview] " << FormatViewTargetCensus( ViewTargetCensus( kPreviewViewProfile, kW, kH ), kW, kH );
    SUCCEED();
}

TEST( ViewMemory, MainViewBytesPerPixelIsPinned )
{
    // 64 scene targets + 72 post stack + 17.8 half/quarter chains and fog + 9 clouds + 24 SSR + 24 GI.
    EXPECT_NEAR( ViewBytesPerPixel( kSceneViewProfile, kW, kH ), 210.83, 0.01 );
    // Four 2048 cascades (R32F + D24S8, 128 MiB) and the 512 RSM (10 MiB).
    EXPECT_EQ( SumViewTargets( ViewTargetCensus( kSceneViewProfile, kW, kH ) ).FixedBytes, 144703488u );
}

TEST( ViewMemory, PreviewViewBytesPerPixelIsPinned )
{
    EXPECT_NEAR( ViewBytesPerPixel( kPreviewViewProfile, kW, kH ), 162.83, 0.01 );
    // One 1024 cascade (R32F + D24S8).
    EXPECT_EQ( SumViewTargets( ViewTargetCensus( kPreviewViewProfile, kW, kH ) ).FixedBytes, 8388608u );
}

TEST( ViewMemory, PreviewIsSmallerThanMainByTheMeasuredAmount )
{
    const uint64_t main    = SumViewTargets( ViewTargetCensus( kSceneViewProfile, kW, kH ) ).Total();
    const uint64_t preview = SumViewTargets( ViewTargetCensus( kPreviewViewProfile, kW, kH ) ).Total();
    ASSERT_GT( main, preview );
    // 96 B/px (SSR 48, GI 48) over 1920x1080 + (128 MiB - 8 MiB) of cascades + the 14 MiB RSM.
    EXPECT_EQ( main - preview, 235847680u ) << "main " << main << " preview " << preview;
}

TEST( ViewMemory, ShadowRowsAgreeWithTheShadowBudgetSpelling )
{
    // ShadowAttachmentBytes is the existing single spelling of cascade cost; the census must not be a second.
    for ( const ViewProfile& profile : { kSceneViewProfile, kPreviewViewProfile, kThumbnailViewProfile } )
    {
        uint64_t shadowBytes = 0;
        for ( const auto& row : ViewTargetCensus( profile, kW, kH ) )
            if ( row.Name.starts_with( "ShadowCascades" ) )
                shadowBytes += row.Bytes();
        EXPECT_EQ( shadowBytes, ShadowAttachmentBytes( profile.Shadows ) );
    }
}

TEST( ViewMemory, PreviewDropsOnlyTheRowsItsProfileNames )
{
    const auto mainRows = RowNames( kSceneViewProfile );
    for ( const auto& name : RowNames( kPreviewViewProfile ) )
    {
        EXPECT_NE( std::find( mainRows.begin(), mainRows.end(), name ), mainRows.end() ) << name;
        EXPECT_FALSE( name.starts_with( "SSR" ) || name.starts_with( "GI" ) || name.starts_with( "RSM" ) ) << name;
    }
}

TEST( ViewMemory, ViewScaledBytesScaleWithPixels )
{
    // Four times the pixels, the same bytes per pixel (to within mip-chain rounding).
    EXPECT_NEAR( ViewBytesPerPixel( kSceneViewProfile, kW, kH ),
                 ViewBytesPerPixel( kSceneViewProfile, 2 * kW, 2 * kH ), 0.05 );
}

// RT1h: a view built before its surface has a size is built at kUnsizedViewExtent. Every target the census
// names must still exist there (a zero-sized image is a device error, not a small allocation), and the whole
// view must cost what the constant's comment says — under a mebibyte — instead of the window's 1.3-2.1 GiB.
TEST( ViewMemory, TheUnsizedViewBuildsEveryTargetForUnderAMebibyte )
{
    using namespace Desert::Graphic;
    const auto rows = ViewTargetCensus( kSceneViewProfile, kUnsizedViewExtent.Width, kUnsizedViewExtent.Height );
    ASSERT_FALSE( rows.empty() );
    for ( const auto& row : rows )
    {
        EXPECT_GT( row.Width, 0u ) << row.Name;
        EXPECT_GT( row.Height, 0u ) << row.Name;
        if ( row.ScalesWithView )
        {
            EXPECT_LE( row.Width, kUnsizedViewExtent.Width ) << row.Name;
            EXPECT_LE( row.Height, kUnsizedViewExtent.Height ) << row.Name;
        }
    }
    EXPECT_LT( SumViewTargets( rows ).ViewScaledBytes, 1ull << 20 );
}

// A view built for a W x H surface allocates its full-resolution targets at exactly W x H: the census the
// renderer logs at its build is taken at the extent the renderer was constructed with.
TEST( ViewMemory, AViewportSizedViewHoldsTargetsAtTheViewportSize )
{
    using namespace Desert::Graphic;
    constexpr ViewExtent viewport{ 1280, 720 };
    const auto           rows    = ViewTargetCensus( kSceneViewProfile, viewport.Width, viewport.Height );
    bool                 sawFull = false;
    for ( const auto& row : rows )
    {
        if ( !row.ScalesWithView )
            continue;
        EXPECT_LE( row.Width, viewport.Width ) << row.Name;
        EXPECT_LE( row.Height, viewport.Height ) << row.Name;
        sawFull = sawFull || ( row.Width == viewport.Width && row.Height == viewport.Height );
    }
    EXPECT_TRUE( sawFull );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
