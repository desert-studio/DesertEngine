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

// ---- WHAT SIZE A VIEW MAY BE BUILT AT (RT1i) ----
//
// The crash this pins: minimising the editor on Windows produced VK_ERROR_INITIALIZATION_FAILED out of
// vmaCreateImage and a __debugbreak inside VK_CHECK_RESULT. Every unusable size below reached the allocator
// because SceneRenderer::Resize tested `width == 0 && height == 0` -- an AND, so a panel collapsed on one
// axis and a panel handing out a negative ImGui float both went straight through.

// A collapsed dock panel is zero on ONE side, which is exactly what the old `&&` let through.
TEST( ViewExtentRule, ZeroOnEitherSideIsNotBuildable )
{
    using namespace Desert::Graphic;
    EXPECT_FALSE( IsUsableViewExtent( 0, 0 ) ) << "a minimised window";
    EXPECT_FALSE( IsUsableViewExtent( 1280, 0 ) ) << "a panel collapsed vertically";
    EXPECT_FALSE( IsUsableViewExtent( 0, 720 ) ) << "a panel collapsed horizontally";
    EXPECT_TRUE( IsUsableViewExtent( 1, 1 ) ) << "one pixel is small, not unusable";
    EXPECT_TRUE( IsUsableViewExtent( 1280, 720 ) );
}

// ImGui reports a NEGATIVE float size for a panel being dragged shut; the cast to uint32_t makes it enormous,
// and an enormous image is refused by the allocator exactly like a zero-pixel one.
TEST( ViewExtentRule, AnAbsurdSizeFromANegativeImGuiFloatIsNotBuildable )
{
    using namespace Desert::Graphic;
    // Written as the NUMBERS the engine's own cast produces rather than as the cast itself: converting a
    // negative float to an unsigned integer is undefined behaviour, so a test that performed it would be
    // asking the compiler what the defect looks like instead of stating it.
    constexpr uint32_t kFromNegativeOne   = 4294967295U; // (uint32_t)-1.0f
    constexpr uint32_t kFromNegativeEight = 4294967288U; // (uint32_t)-8.0f
    EXPECT_FALSE( IsUsableViewExtent( kFromNegativeOne, 720 ) );
    EXPECT_FALSE( IsUsableViewExtent( 1280, kFromNegativeEight ) );
    EXPECT_FALSE( IsUsableViewExtent( kMaxViewExtentSide + 1, kMaxViewExtentSide ) );
    EXPECT_TRUE( IsUsableViewExtent( kMaxViewExtentSide, kMaxViewExtentSide ) ) << "the bound itself is usable";
}

// The swapchain's own rule: the SURFACE decides, which is why rebuilding at the window's last known size
// still ends up 0x0 while the window is minimised.
TEST( ViewExtentRule, AMinimisedSurfaceOverridesWhateverSizeWasAskedFor )
{
    using namespace Desert::Graphic;
    constexpr ViewExtent lastKnownGood{ 1600, 900 };
    const ViewExtent     minimised = ResolveSurfaceExtent( ViewExtent{ 0, 0 }, lastKnownGood );
    EXPECT_EQ( minimised.Width, 0U );
    EXPECT_EQ( minimised.Height, 0U );
    EXPECT_FALSE( IsUsableViewExtent( minimised ) ) << "the rebuild must be skipped, not attempted at 0x0";

    const ViewExtent restored = ResolveSurfaceExtent( ViewExtent{ 1280, 720 }, lastKnownGood );
    EXPECT_EQ( restored.Width, 1280U ) << "the surface's extent wins over the requested one";
    EXPECT_EQ( restored.Height, 720U );
    EXPECT_TRUE( IsUsableViewExtent( restored ) );
}

// The one case where the surface genuinely defers to the caller (0xFFFFFFFF on both sides).
TEST( ViewExtentRule, ASurfaceWithNoExtentOfItsOwnTakesTheRequestedOne )
{
    using namespace Desert::Graphic;
    const ViewExtent resolved = ResolveSurfaceExtent(
         ViewExtent{ kSurfaceExtentUndefined, kSurfaceExtentUndefined }, ViewExtent{ 1024, 768 } );
    EXPECT_EQ( resolved.Width, 1024U );
    EXPECT_EQ( resolved.Height, 768U );
    EXPECT_TRUE( IsUsableViewExtent( resolved ) );
}

// ── THE FRAME LOOP STANDS DOWN WHILE THE WINDOW HAS NO DRAWABLE AREA ────────────────────────────────
//
// The relation these hold, which no single value can state: the gate's answer to ONE frame is not the
// point — the point is what it answers over a SEQUENCE of them. A skip that announced itself every frame
// is what the previous behaviour did (72 identical errors in one live run of three minimises), and a
// skip that announced itself once and then never again would go silent on the second minimise.

// Every frame of a minimise is skipped, and exactly the FIRST of them is announced.
TEST( DrawableArea, EveryMinimisedFrameIsSkippedAndOnlyTheFirstIsAnnounced )
{
    using namespace Desert::Graphic;
    DrawableAreaGate gate;

    ASSERT_FALSE( gate.Observe( true ).SkipFrame ) << "a drawable window must not be stood down";

    const auto first = gate.Observe( false );
    EXPECT_TRUE( first.SkipFrame );
    EXPECT_TRUE( first.AnnounceStop ) << "the minimise has to be said once, or nobody can see why frames stopped";

    for ( int frame = 0; frame < 200; ++frame )
    {
        const auto later = gate.Observe( false );
        EXPECT_TRUE( later.SkipFrame );
        EXPECT_FALSE( later.AnnounceStop ) << "frame " << frame << " announced a minimise that was already on";
        EXPECT_FALSE( later.AnnounceResume );
    }
    EXPECT_TRUE( gate.IsSkipping() );
}

// Coming back is announced once too, and the frame that announces it RUNS — the resume is not itself a
// skipped frame, or the first frame after every restore would be dropped.
TEST( DrawableArea, TheRestoreIsAnnouncedOnceAndItsFrameRuns )
{
    using namespace Desert::Graphic;
    DrawableAreaGate gate;

    (void)gate.Observe( false );
    (void)gate.Observe( false );

    const auto restored = gate.Observe( true );
    EXPECT_FALSE( restored.SkipFrame ) << "the frame that sees the area come back must run";
    EXPECT_TRUE( restored.AnnounceResume );
    EXPECT_FALSE( restored.AnnounceStop );
    EXPECT_FALSE( gate.IsSkipping() );

    const auto after = gate.Observe( true );
    EXPECT_FALSE( after.SkipFrame );
    EXPECT_FALSE( after.AnnounceResume ) << "a resume announced every frame is the flood in the other direction";
}

// THE SECOND MINIMISE IS ANNOUNCED AS LOUDLY AS THE FIRST. A gate that latched "already said it" would
// pass both tests above and still leave the log silent for every minimise after the first.
TEST( DrawableArea, EveryMinimiseIsAnnouncedNotOnlyTheFirstOne )
{
    using namespace Desert::Graphic;
    DrawableAreaGate gate;

    int announcedStops = 0;
    int announcedResumes = 0;
    for ( int cycle = 0; cycle < 3; ++cycle )
    {
        for ( int frame = 0; frame < 5; ++frame )
        {
            if ( gate.Observe( false ).AnnounceStop )
                ++announcedStops;
        }
        for ( int frame = 0; frame < 5; ++frame )
        {
            if ( gate.Observe( true ).AnnounceResume )
                ++announcedResumes;
        }
    }

    // Three minimise/restore cycles, thirty frames: six lines, not thirty and not two.
    EXPECT_EQ( announcedStops, 3 );
    EXPECT_EQ( announcedResumes, 3 );
}

// The gate and the swapchain must agree on what "minimised" means, and the extent rule is what makes
// them agree: the bool the loop gates on is IsUsableViewExtent of the framebuffer the window reports
// (Platform/*/*.cpp HasDrawableArea), the same predicate Rebuild refuses on.
TEST( DrawableArea, TheGateIsFedByTheSameExtentRuleTheSwapchainRefusesOn )
{
    using namespace Desert::Graphic;
    DrawableAreaGate gate;

    const ViewExtent minimised{ 0, 0 };
    const ViewExtent restored{ 1280, 720 };

    EXPECT_TRUE( gate.Observe( IsUsableViewExtent( restored ) ).SkipFrame == false );
    EXPECT_TRUE( gate.Observe( IsUsableViewExtent( minimised ) ).SkipFrame )
         << "a frame must not run for an extent the swapchain would refuse to rebuild at";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
