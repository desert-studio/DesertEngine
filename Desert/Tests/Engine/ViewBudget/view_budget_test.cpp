// The view budget (Engine/Core/ViewBudget.hpp): who may create a view, decided in BYTES.
//
// A refusal needs a device near its budget, which no suite can arrange and a frame can reach only through
// `--view-budget-mib`; the rule itself is pure, so every branch is pinned here without a GPU.

#include <Engine/Core/ViewBudget.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace
{
    namespace VB = Desert::Engine::ViewBudget;

    constexpr uint64_t kMiB = 1024ull * 1024ull;

    VB::Reading Known( const uint64_t ceiling, const uint64_t usage )
    {
        return VB::ReadCeiling( true, ceiling, usage, /*deviceLocalHeapSize=*/ceiling * 4, /*overrideBytes=*/0,
                                /*heldByViews=*/0 );
    }
} // namespace

TEST( ViewBudget, BackgroundKeepsTheReserveInBytesAndAUserSurfaceDoesNot )
{
    // 1000 MiB ceiling, 200 in use: 800 free. A 300 MiB view with a 500 MiB reserve fits exactly.
    const VB::Reading reading = Known( 1000 * kMiB, 200 * kMiB );

    EXPECT_TRUE( VB::MayCreate( VB::Demand::Background, 300 * kMiB, 500 * kMiB, reading ).Ok )
         << "request + reserve == free must fit";
    const VB::Verdict over = VB::MayCreate( VB::Demand::Background, 300 * kMiB + 1, 500 * kMiB, reading );
    EXPECT_FALSE( over.Ok ) << "one byte past the reserve and background work still took it";
    EXPECT_EQ( over.ReserveBytes, 500 * kMiB );

    const VB::Verdict user = VB::MayCreate( VB::Demand::UserSurface, 800 * kMiB, 500 * kMiB, reading );
    EXPECT_TRUE( user.Ok ) << "a user surface may take the last byte; the reserve is background work's alone";
    EXPECT_EQ( user.ReserveBytes, 0u );
    EXPECT_FALSE( VB::MayCreate( VB::Demand::UserSurface, 800 * kMiB + 1, 0, reading ).Ok )
         << "a user surface was granted more than is free";
}

TEST( ViewBudget, UsageAboveTheCeilingGrantsNothingRatherThanWrappingAround )
{
    const VB::Reading reading = Known( 100 * kMiB, 150 * kMiB );
    const VB::Verdict verdict = VB::MayCreate( VB::Demand::UserSurface, 1, 0, reading );
    EXPECT_FALSE( verdict.Ok );
    EXPECT_EQ( verdict.FreeBytes, 0u );
    EXPECT_FALSE(
         VB::MayCreate( VB::Demand::Background, 0, std::numeric_limits<uint64_t>::max(), Known( 100 * kMiB, 0 ) )
              .Ok );
}

TEST( ViewBudget, ARefusalNamesRequestCeilingUsageAndEveryOpenView )
{
    const VB::Reading reading = Known( 1024 * kMiB, 900 * kMiB );
    const VB::Verdict verdict = VB::MayCreate( VB::Demand::Background, 256 * kMiB, 128 * kMiB, reading );
    ASSERT_FALSE( verdict.Ok );

    const std::string text =
         VB::DescribeRefusal( "asset thumbnail", verdict, reading,
                              { { "renderer slot 0", 600 * kMiB }, { "renderer slot 1", 64 * kMiB } } );
    for ( const char* piece : { "View 'asset thumbnail' needs 256.0 MiB", "budget 1024.0 MiB", "in use 900.0 MiB",
                                "keeps 128.0 MiB free", "open views hold 664.0 MiB", "renderer slot 0 — 600.0 MiB",
                                "renderer slot 1 — 64.0 MiB" } )
        EXPECT_NE( text.find( piece ), std::string::npos ) << "missing '" << piece << "' in: " << text;
    EXPECT_EQ( text.find( "unknown" ), std::string::npos ) << "a known budget was described as unknown: " << text;
}

TEST( ViewBudget, AnUnknownBudgetUsesTheHeapSizeAndSaysSo )
{
    // No VK_EXT_memory_budget: the driver's budget/usage fields are 0 and mean nothing.
    const VB::Reading reading =
         VB::ReadCeiling( false, 0, 0, /*deviceLocalHeapSize=*/2048 * kMiB, 0, /*heldByViews=*/300 * kMiB );
    EXPECT_EQ( reading.Source, VB::CeilingSource::HeapSize );
    EXPECT_EQ( reading.CeilingBytes, 2048 * kMiB ) << "the ceiling must be the device-local heap size";
    EXPECT_FALSE( reading.UsageKnown );
    EXPECT_EQ( reading.UsageBytes, 300 * kMiB ) << "usage must stand in as what the open views hold, not 0";

    const VB::Verdict verdict = VB::MayCreate( VB::Demand::UserSurface, 4096 * kMiB, 0, reading );
    ASSERT_FALSE( verdict.Ok );
    const std::string text = VB::DescribeRefusal( "Details preview", verdict, reading, {} );
    EXPECT_NE( text.find( "budget unknown" ), std::string::npos ) << text;
    EXPECT_NE( text.find( "ceiling = device-local heap size 2048.0 MiB" ), std::string::npos ) << text;
    EXPECT_NE( text.find( "counted from open views only" ), std::string::npos ) << text;
}

TEST( ViewBudget, TheCommandLineCeilingReplacesTheDriversAndIsNamed )
{
    const VB::Reading reading = VB::ReadCeiling( true, 16384 * kMiB, 500 * kMiB, 16384 * kMiB, 512 * kMiB, 0 );
    EXPECT_EQ( reading.Source, VB::CeilingSource::CommandLine );
    EXPECT_EQ( reading.CeilingBytes, 512 * kMiB );
    EXPECT_EQ( reading.UsageBytes, 500 * kMiB ) << "the override moves the ceiling, not the usage";
    EXPECT_FALSE( VB::MayCreate( VB::Demand::UserSurface, 13 * kMiB, 0, reading ).Ok );
    EXPECT_NE( VB::DescribeCeiling( reading ).find( "--view-budget-mib" ), std::string::npos );
}

// A LIVE VIEW'S RESIZE IS NEVER REFUSED (RT2l). The inputs are the editor's own at `--view-budget-mib 250`:
// the main view built at its 64x64 placeholder and resized to its 996x504 panel with 328 MiB already in use.
// The resize gets its size and only reports how far past the ceiling it went -- the growth minus what was
// free, not the whole growth and not the whole new size; a shrink never overruns even on a full device. At
// the same reading a NEW document is still refused: admission is where the budget says no.
TEST( ViewBudget, ALiveViewsResizeIsNeverRefusedAndReportsOnlyItsOverrun )
{
    const VB::Reading full = Known( 250 * kMiB, 328 * kMiB );
    EXPECT_EQ( VB::ResizeOverrunBytes( 1 * kMiB, 139 * kMiB, full ), 138 * kMiB )
         << "nothing was free, so the whole growth is the overrun";
    EXPECT_EQ( VB::ResizeOverrunBytes( 139 * kMiB, 1 * kMiB, full ), 0u ) << "a shrink overran a full device";
    EXPECT_FALSE( VB::MayCreate( VB::Demand::UserSurface, 74 * kMiB, 0, full ).Ok )
         << "a new document must still be refused where a live view's resize is not";

    const VB::Reading roomy = Known( 100 * kMiB, 90 * kMiB );
    EXPECT_EQ( VB::ResizeOverrunBytes( 40 * kMiB, 50 * kMiB, roomy ), 0u ) << "growth of exactly what is free";
    EXPECT_EQ( VB::ResizeOverrunBytes( 40 * kMiB, 51 * kMiB, roomy ), 1 * kMiB )
         << "the overrun is the growth minus what was free";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
