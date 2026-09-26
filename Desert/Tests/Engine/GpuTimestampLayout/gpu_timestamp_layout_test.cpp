// Where GPU timestamps live in a frame's query pool, how that pool grows, and whether a breakdown built
// from them adds up.
//
// Four relations, the first three new with the pool-per-frame profiler and the last one already wrong once:
//
//   1. Linear hand-out. Scope i owns queries 2+2i and 3+2i; the whole-frame bracket owns 0 and 1. No two
//      scopes and no scope and the bracket share a query, however many views record into the frame —
//      there is no view dimension in the layout, so there is no ceiling on views.
//
//   2. Growth by high-water mark. The pool fits the busiest frame so far, never shrinks, and grows
//      geometrically so a creeping scope count does not reallocate every frame.
//
//   3. Parents never cross views. Views' scopes interleave in one list; a preview's pass must not become
//      the child of the viewport's open scope, or the viewport's self time would lose the preview's cost.
//
//   4. Self times partition the root. Passes nest, so summing the INCLUSIVE column counts a parent's
//      microseconds again in every child: the first breakdown this feature ever printed came to 159 % of
//      its own frame. Subtracting direct children makes the remainder a partition.
//
// Nothing here touches Vulkan.

#include <Engine/Graphic/GpuTimestampLayout.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using Desert::Graphic::GpuGrownQueryCount;
using Desert::Graphic::GpuQueriesForScopes;
using Desert::Graphic::GpuScopeCapacity;
using Desert::Graphic::GpuScopeQueryBase;
using Desert::Graphic::GpuScopeRecorder;
using Desert::Graphic::GpuSelfTimes;
using Desert::Graphic::GpuViewScopeName;
using Desert::Graphic::kGpuFrameTotalQuery;
using Desert::Graphic::kGpuInitialScopeCapacity;
using Desert::Graphic::kGpuNoParent;
using Desert::Graphic::kGpuQueryGranularity;

// --- 1. linear hand-out --------------------------------------------------------------------------------

// Every query a pool of N scopes can be asked for is claimed exactly once, and the last one is inside the
// pool GpuQueriesForScopes sized.
TEST( GpuTimestampLayout, EveryQueryBelongsToExactlyOneScopeOrTheFrameBracket )
{
    constexpr uint32_t kScopes = 300; // eight views of ~30 passes, and then some

    std::set<uint32_t> seen{ kGpuFrameTotalQuery, kGpuFrameTotalQuery + 1 };
    for ( uint32_t scope = 0; scope < kScopes; ++scope )
    {
        const uint32_t begin = GpuScopeQueryBase( scope );
        EXPECT_TRUE( seen.insert( begin ).second ) << "scope " << scope << " begin collides";
        EXPECT_TRUE( seen.insert( begin + 1 ).second ) << "scope " << scope << " end collides";
    }

    EXPECT_EQ( seen.size(), static_cast<size_t>( GpuQueriesForScopes( kScopes ) ) );
    EXPECT_EQ( *seen.rbegin(), GpuQueriesForScopes( kScopes ) - 1 ); // dense: nothing wasted, nothing past the end
}

TEST( GpuTimestampLayout, CapacityInvertsTheQueryCount )
{
    for ( uint32_t scopes = 0; scopes < 1000; ++scopes )
        EXPECT_EQ( GpuScopeCapacity( GpuQueriesForScopes( scopes ) ), scopes );

    // A pool too small for even the bracket times no scope, rather than underflowing to four billion.
    EXPECT_EQ( GpuScopeCapacity( 0 ), 0u );
    EXPECT_EQ( GpuScopeCapacity( 1 ), 0u );
    // An odd leftover query is not half a scope.
    EXPECT_EQ( GpuScopeCapacity( GpuQueriesForScopes( 5 ) + 1 ), 5u );
}

// --- 2. growth ---------------------------------------------------------------------------------------

TEST( GpuTimestampLayout, TheFirstPoolFitsTheInitialCapacity )
{
    const uint32_t first = GpuGrownQueryCount( 0, kGpuInitialScopeCapacity );
    EXPECT_GE( GpuScopeCapacity( first ), kGpuInitialScopeCapacity );
    EXPECT_EQ( first % kGpuQueryGranularity, 0u );
}

// The relation growth exists for: after growing to a high-water mark, every scope of that frame fits.
TEST( GpuTimestampLayout, GrownPoolFitsTheHighWaterMark )
{
    uint32_t queries = GpuGrownQueryCount( 0, kGpuInitialScopeCapacity );
    for ( const uint32_t highWater : { 10u, 64u, 65u, 97u, 240u, 241u, 1000u, 5000u } )
    {
        queries = GpuGrownQueryCount( queries, highWater );
        EXPECT_GE( GpuScopeCapacity( queries ), highWater ) << "high water " << highWater;
        EXPECT_EQ( queries % kGpuQueryGranularity, 0u ) << "high water " << highWater;
    }
}

// A frame with fewer scopes than the pool holds (a view closed) keeps the pool: shrinking would make the
// next frame with that view drop scopes and grow all over again.
TEST( GpuTimestampLayout, PoolNeverShrinks )
{
    const uint32_t big = GpuGrownQueryCount( 0, 1000 );
    EXPECT_EQ( GpuGrownQueryCount( big, 0 ), big );
    EXPECT_EQ( GpuGrownQueryCount( big, 1 ), big );
    EXPECT_EQ( GpuGrownQueryCount( big, GpuScopeCapacity( big ) ), big ); // exactly full still fits
}

// Opening views one at a time raises the high-water mark by ~30 scopes a step. The pool must not be
// replaced on every step: geometric growth bounds the replacements by a logarithm of the final size.
TEST( GpuTimestampLayout, CreepingDemandReallocatesLogarithmically )
{
    uint32_t queries      = GpuGrownQueryCount( 0, kGpuInitialScopeCapacity );
    uint32_t reallocation = 0;
    for ( uint32_t highWater = 1; highWater <= 4096; ++highWater )
    {
        const uint32_t grown = GpuGrownQueryCount( queries, highWater );
        if ( grown != queries )
        {
            EXPECT_GE( grown, queries + queries / 2 ) << "grew by less than half at high water " << highWater;
            ++reallocation;
        }
        queries = grown;
    }
    EXPECT_LE( reallocation, 12u ); // 192 -> ~8200 queries at x1.5 is ten steps; one scope per step would be 4000
}

// --- 3. per-view nesting in one list -------------------------------------------------------------------

namespace
{
    // Stand-ins for two ViewResources and the frame context: the recorder only compares the addresses.
    int kViewport = 0;
    int kPreview  = 0;
    int kFrameCtx = 0;
} // namespace

// Two views record one after the other into the same frame, each with nested passes; the frame context's
// UI scope is open around both. No parent may point into another owner's scopes.
TEST( GpuTimestampLayout, ParentsNeverCrossViews )
{
    GpuScopeRecorder rec;
    rec.Reset( 64 );

    const int32_t ui       = rec.Begin( "UI", &kFrameCtx );
    const int32_t vpUpdate = rec.Begin( "OnUpdate", &kViewport );
    const int32_t vpClouds = rec.Begin( "VolumetricClouds", &kViewport );
    // The preview opens while the viewport's two scopes are still open (a phase that records another view
    // from inside its own) — it must start a root of its own, not become the viewport's grandchild.
    const int32_t pvUpdate = rec.Begin( "OnUpdate", &kPreview );
    const int32_t pvClouds = rec.Begin( "VolumetricClouds", &kPreview );
    rec.End( pvClouds );
    rec.End( pvUpdate );
    rec.End( vpClouds );
    const int32_t vpPost = rec.Begin( "PostProcess", &kViewport );
    rec.End( vpPost );
    rec.End( vpUpdate );
    rec.End( ui );

    const auto& scopes = rec.Scopes();
    ASSERT_EQ( scopes.size(), 6u );
    EXPECT_EQ( scopes[ui].Parent, kGpuNoParent );
    EXPECT_EQ( scopes[vpUpdate].Parent, kGpuNoParent ) << "a view's root must not hang under the frame context";
    EXPECT_EQ( scopes[vpClouds].Parent, vpUpdate );
    EXPECT_EQ( scopes[pvUpdate].Parent, kGpuNoParent ) << "the preview became a child of the viewport";
    EXPECT_EQ( scopes[pvClouds].Parent, pvUpdate );
    EXPECT_EQ( scopes[vpPost].Parent, vpUpdate ) << "closing the preview's scopes disturbed the viewport's stack";

    for ( size_t i = 0; i < scopes.size(); ++i )
        if ( scopes[i].Parent != kGpuNoParent )
            EXPECT_LT( scopes[i].Parent, static_cast<int32_t>( i ) ) << "GpuSelfTimes needs parents first";
}

// A full pool refuses the extra scopes but COUNTS them — the count is the high-water mark the pool grows
// to — and a refused scope's -1 does not disturb the nesting of the ones that were timed.
TEST( GpuTimestampLayout, RefusedScopesRaiseTheHighWaterAndLeaveNestingAlone )
{
    GpuScopeRecorder rec;
    rec.Reset( 2 );

    const int32_t outer   = rec.Begin( "outer", &kViewport );
    const int32_t inner   = rec.Begin( "inner", &kViewport );
    const int32_t refused = rec.Begin( "refused", &kViewport );
    EXPECT_EQ( refused, -1 );
    rec.End( refused );
    rec.End( inner );

    EXPECT_EQ( rec.Scopes().size(), 2u );
    EXPECT_EQ( rec.Requested(), 3u );
    EXPECT_EQ( rec.Scopes()[inner].Parent, outer );

    // The next frame at the grown size takes all three.
    rec.Reset( GpuScopeCapacity( GpuGrownQueryCount( GpuQueriesForScopes( 2 ), rec.Requested() ) ) );
    EXPECT_GE( rec.Begin( "outer", &kViewport ), 0 );
    EXPECT_GE( rec.Begin( "inner", &kViewport ), 0 );
    EXPECT_GE( rec.Begin( "refused", &kViewport ), 0 );
}

// The acceptance shape without a device: eight views of thirty passes each. After one growth every view's
// every pass has its own row name and its own query pair.
TEST( GpuTimestampLayout, EightViewsAllGetRowsAfterOneGrowth )
{
    constexpr uint32_t kViews  = 8;
    constexpr uint32_t kPasses = 30;
    int                views[kViews]{};

    uint32_t         queries = GpuGrownQueryCount( 0, kGpuInitialScopeCapacity );
    GpuScopeRecorder rec;

    const auto recordFrame = [&]
    {
        rec.Reset( GpuScopeCapacity( queries ) );
        for ( uint32_t v = 0; v < kViews; ++v )
            for ( uint32_t p = 0; p < kPasses; ++p )
                rec.End(
                     rec.Begin( GpuViewScopeName( "view " + std::to_string( v ), "pass " + std::to_string( p ) ),
                                &views[v] ) );
    };

    recordFrame();
    EXPECT_LT( rec.Scopes().size(), kViews * kPasses ) << "the first pool should be too small for eight views";
    queries = GpuGrownQueryCount( queries, rec.Requested() );

    recordFrame();
    ASSERT_EQ( rec.Scopes().size(), kViews * kPasses );
    std::set<std::string> rows;
    for ( const auto& scope : rec.Scopes() )
        rows.insert( scope.Name );
    EXPECT_EQ( rows.size(), static_cast<size_t>( kViews * kPasses ) ) << "two views' passes share a profiler row";
    EXPECT_LE( GpuScopeQueryBase( kViews * kPasses - 1 ) + 1, queries - 1 );
}

TEST( GpuTimestampLayout, FrameContextScopesKeepTheirBareName )
{
    EXPECT_EQ( GpuViewScopeName( "", "UI" ), "UI" );
    EXPECT_EQ( GpuViewScopeName( "Viewport", "VolumetricClouds" ), "Viewport | VolumetricClouds" );
}

// --- 4. self times partition the root ----------------------------------------------------------------

namespace
{
    double Sum( const std::vector<double>& v )
    {
        double total = 0.0;
        for ( double x : v )
            total += x;
        return total;
    }
} // namespace

// The frame's real shape: VolumetricClouds > Clouds: ExecuteInFrame > {March, TemporalResolve}. Summing
// the inclusive column here gives 8.0 + 7.9 + 7.1 + 0.7 = 23.7 ms out of an 8.0 ms pass; the self times
// give back exactly 8.0.
TEST( GpuTimestampLayout, SelfTimesSumToTheRootInclusive )
{
    //                       0: VolumetricClouds  1: ExecuteInFrame  2: March  3: Resolve
    const std::vector<double>  inclusive{ 8.000, 7.900, 7.100, 0.700 };
    const std::vector<int32_t> parents{ kGpuNoParent, 0, 1, 1 };

    const std::vector<double> self = GpuSelfTimes( inclusive, parents );

    // NEAR, not DOUBLE_EQ, on the SUBTRACTIONS: 8.000 - 7.900 is 0.09999999999999964 in binary floating
    // point, and a breakdown in milliseconds has no interest in the sixteenth digit. The leaves below are
    // never subtracted from, so those stay exact and are asserted exactly.
    EXPECT_NEAR( self[0], 0.100, 1e-12 ); // 8.000 - 7.900
    EXPECT_NEAR( self[1], 0.100, 1e-12 ); // 7.900 - 7.100 - 0.700
    EXPECT_DOUBLE_EQ( self[2], 7.100 );   // leaves keep their own time
    EXPECT_DOUBLE_EQ( self[3], 0.700 );
    EXPECT_NEAR( Sum( self ), inclusive[0], 1e-12 ); // the partition
}

// Siblings at the top level are not each other's children, so the total is their sum — this is what makes
// the frame's own bracket comparable with the passes under it.
TEST( GpuTimestampLayout, IndependentRootsAreNotSubtractedFromEachOther )
{
    const std::vector<double>  inclusive{ 4.7, 7.1, 0.6 };
    const std::vector<int32_t> parents{ kGpuNoParent, kGpuNoParent, kGpuNoParent };

    const std::vector<double> self = GpuSelfTimes( inclusive, parents );

    EXPECT_DOUBLE_EQ( self[0], 4.7 );
    EXPECT_DOUBLE_EQ( self[1], 7.1 );
    EXPECT_DOUBLE_EQ( self[2], 0.6 );
    EXPECT_NEAR( Sum( self ), 12.4, 1e-12 );
}

// A scope whose two queries did not both land is marked -1 and must not be charged to its parent: doing
// so would inflate a sibling's apparent cost by a pass that was never measured.
TEST( GpuTimestampLayout, UnresolvedScopesDoNotDisturbTheirParent )
{
    const std::vector<double>  inclusive{ 5.0, -1.0, 2.0 };
    const std::vector<int32_t> parents{ kGpuNoParent, 0, 0 };

    const std::vector<double> self = GpuSelfTimes( inclusive, parents );

    EXPECT_DOUBLE_EQ( self[0], 3.0 ); // 5.0 - 2.0, the -1 child ignored
    EXPECT_LT( self[1], 0.0 );        // stays marked unresolved
    EXPECT_DOUBLE_EQ( self[2], 2.0 );
}

// A child measured slightly longer than its parent (the two timestamps are separate writes, so this is
// possible at the microsecond level) must clamp to zero rather than go negative — a negative self time
// would quietly shrink the total and read as unattributed GPU work that does not exist.
TEST( GpuTimestampLayout, SelfTimeNeverGoesNegative )
{
    const std::vector<double>  inclusive{ 1.000, 1.002 };
    const std::vector<int32_t> parents{ kGpuNoParent, 0 };

    const std::vector<double> self = GpuSelfTimes( inclusive, parents );

    EXPECT_DOUBLE_EQ( self[0], 0.0 );
    EXPECT_DOUBLE_EQ( self[1], 1.002 );
}

// Deep nesting: the engine really does reach four levels (frame > SceneRenderer::OnUpdate >
// VolumetricClouds > Clouds: ExecuteInFrame > Clouds: March), and only DIRECT children may be
// subtracted — subtracting grandchildren twice was the other way to get this wrong.
TEST( GpuTimestampLayout, OnlyDirectChildrenAreSubtracted )
{
    const std::vector<double>  inclusive{ 10.0, 9.0, 8.0, 7.0 };
    const std::vector<int32_t> parents{ kGpuNoParent, 0, 1, 2 };

    const std::vector<double> self = GpuSelfTimes( inclusive, parents );

    EXPECT_DOUBLE_EQ( self[0], 1.0 );
    EXPECT_DOUBLE_EQ( self[1], 1.0 );
    EXPECT_DOUBLE_EQ( self[2], 1.0 );
    EXPECT_DOUBLE_EQ( self[3], 7.0 );
    EXPECT_NEAR( Sum( self ), inclusive[0], 1e-12 );
}

// Only gtest is linked, not gtest_main — every suite in this tree brings its own entry point.
int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
