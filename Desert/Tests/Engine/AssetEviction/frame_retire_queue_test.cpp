// WP13: an evicted mesh's GPU buffers outlive the sweep until no frame in flight can still draw them.
// Assets::FrameRetireQueue is the rule MeshService::EvictBuilt parks into; a shared_ptr stands in for the mesh and
// a weak_ptr observes when it is really destroyed.
#include <Engine/Assets/FrameRetireQueue.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace
{
    using Desert::Assets::FrameRetireQueue;

    struct StandInMesh
    {
        int Buffers = 2;
    };

    constexpr std::uint32_t kFramesInFlight = 2;
} // namespace

TEST( FrameRetireQueue, AMeshIsNotFreedWhileAFrameThatCouldDrawItIsInFlight )
{
    FrameRetireQueue<std::shared_ptr<StandInMesh>> retiring;
    auto                                           mesh     = std::make_shared<StandInMesh>();
    const std::weak_ptr<StandInMesh>               observed = mesh;

    // The sweep at the start of frame 10 drops the mesh: the service lets go of it, the queue keeps it.
    constexpr std::uint64_t kSweepFrame = 10;
    retiring.Park( std::move( mesh ), kSweepFrame );
    ASSERT_FALSE( observed.expired() ) << "parking released the mesh at the sweep";

    // Frame 9 drew it and can still be executing until frame 9 + kFramesInFlight has begun; one frame of margin.
    for ( std::uint64_t frame = kSweepFrame; frame <= kSweepFrame + kFramesInFlight; ++frame )
    {
        EXPECT_EQ( retiring.Collect( frame, kFramesInFlight ), 0u ) << "released at frame " << frame;
        ASSERT_FALSE( observed.expired() )
             << "the buffers were freed at frame " << frame << " while a frame that drew them can be in flight";
    }

    EXPECT_EQ( retiring.Collect( kSweepFrame + kFramesInFlight + 1, kFramesInFlight ), 1u );
    EXPECT_TRUE( observed.expired() ) << "the mesh outlived every frame that could read it";
    EXPECT_EQ( retiring.Size(), 0u );
}

TEST( FrameRetireQueue, EachMeshWaitsForItsOwnSweepNotTheLatest )
{
    FrameRetireQueue<std::shared_ptr<StandInMesh>> retiring;
    auto                                           early         = std::make_shared<StandInMesh>();
    auto                                           late          = std::make_shared<StandInMesh>();
    const std::weak_ptr<StandInMesh>               earlyObserved = early;
    const std::weak_ptr<StandInMesh>               lateObserved  = late;

    retiring.Park( std::move( early ), 100 );
    retiring.Park( std::move( late ), 105 );

    EXPECT_EQ( retiring.Collect( 103, kFramesInFlight ), 1u );
    EXPECT_TRUE( earlyObserved.expired() ) << "a mesh swept at 100 was held back by one swept at 105";
    EXPECT_FALSE( lateObserved.expired() ) << "a mesh swept at 105 was freed with the one swept at 100";

    EXPECT_EQ( retiring.Collect( 108, kFramesInFlight ), 1u );
    EXPECT_TRUE( lateObserved.expired() );
}

TEST( FrameRetireQueue, ClearReleasesEverythingAtOnce )
{
    FrameRetireQueue<std::shared_ptr<StandInMesh>> retiring;
    auto                                           mesh     = std::make_shared<StandInMesh>();
    const std::weak_ptr<StandInMesh>               observed = mesh;
    retiring.Park( std::move( mesh ), 1 );

    retiring.Clear();
    EXPECT_TRUE( observed.expired() );
    EXPECT_EQ( retiring.Size(), 0u );
}
