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
    // 96 scene targets + 108 post stack + 20.5 half/quarter chains and fog + 9 clouds + 48 SSR + 48 GI.
    EXPECT_NEAR( ViewBytesPerPixel( kSceneViewProfile, kW, kH ), 329.49, 0.01 );
    // Four 2048 cascades (RGBA32F + D24S8) and the 512 RSM.
    EXPECT_EQ( SumViewTargets( ViewTargetCensus( kSceneViewProfile, kW, kH ) ).FixedBytes, 350224384u );
}

TEST( ViewMemory, PreviewViewBytesPerPixelIsPinned )
{
    EXPECT_NEAR( ViewBytesPerPixel( kPreviewViewProfile, kW, kH ), 233.49, 0.01 );
    EXPECT_EQ( SumViewTargets( ViewTargetCensus( kPreviewViewProfile, kW, kH ) ).FixedBytes, 20971520u );
}

TEST( ViewMemory, PreviewIsSmallerThanMainByTheMeasuredAmount )
{
    const uint64_t main    = SumViewTargets( ViewTargetCensus( kSceneViewProfile, kW, kH ) ).Total();
    const uint64_t preview = SumViewTargets( ViewTargetCensus( kPreviewViewProfile, kW, kH ) ).Total();
    ASSERT_GT( main, preview );
    // 96 B/px (SSR 48, GI 48) over 1920x1080 + (320 MiB - 20 MiB) of cascades + the 14 MiB RSM.
    EXPECT_EQ( main - preview, 528318464u ) << "main " << main << " preview " << preview;
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

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
