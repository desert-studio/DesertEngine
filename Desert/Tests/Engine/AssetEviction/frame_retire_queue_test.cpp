// WP13: an evicted mesh's GPU buffers outlive the sweep until no frame in flight can still draw them.
// Assets::FrameRetireQueue is the rule MeshService::EvictBuilt parks into; a shared_ptr stands in for the mesh and
// a weak_ptr observes when it is really destroyed.
#include <Engine/Assets/EvictionDeadline.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

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

// WP14b: the same rule for a texture's image, observed the same way. The sweep at frame 20 drops the texture while
// frame 19 - which sampled it - may still be executing; the image must outlive every frame that could sample it,
// and must go once none can.
TEST( FrameRetireQueue, ATextureDroppedBySweepOutlivesEveryFrameThatCouldSampleIt )
{
    struct StandInImage
    {
        int Mips = 11;
    };
    FrameRetireQueue<std::shared_ptr<StandInImage>> retiring;
    auto                                            image    = std::make_shared<StandInImage>();
    const std::weak_ptr<StandInImage>               observed = image;

    constexpr std::uint64_t kSweepFrame = 20;
    retiring.Park( std::move( image ), kSweepFrame );
    for ( std::uint64_t frame = kSweepFrame; frame <= kSweepFrame + kFramesInFlight; ++frame )
    {
        EXPECT_EQ( retiring.Collect( frame, kFramesInFlight ), 0u );
        ASSERT_FALSE( observed.expired() )
             << "the image was freed at frame " << frame << " while a frame that sampled it can be in flight";
    }
    EXPECT_EQ( retiring.Collect( kSweepFrame + kFramesInFlight + 1, kFramesInFlight ), 1u );
    EXPECT_TRUE( observed.expired() ) << "the image outlived every frame that could sample it";
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

TEST( FrameRetireQueue, AMaterialInvalidatedMidRecordingOutlivesTheFrameRecordingIt )
{
    // MaterialService::Invalidate is called mid-UI, while frame F is recording against the material's descriptor
    // pools, and parks it at F. Frame F itself must retire before the pools go.
    FrameRetireQueue<std::shared_ptr<StandInMesh>> graveyard;
    auto                                           material        = std::make_shared<StandInMesh>();
    const std::weak_ptr<StandInMesh>               observed        = material;
    constexpr std::uint64_t                        kRecordingFrame = 40;
    graveyard.Park( std::move( material ), kRecordingFrame );

    for ( std::uint64_t frame = kRecordingFrame; frame <= kRecordingFrame + kFramesInFlight; ++frame )
    {
        graveyard.Collect( frame, kFramesInFlight );
        ASSERT_FALSE( observed.expired() ) << "descriptor pools freed at frame " << frame;
    }
    graveyard.Collect( kRecordingFrame + kFramesInFlight + 1, kFramesInFlight );
    EXPECT_TRUE( observed.expired() );
}

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( prefix + "Desert/Desert/Source/Engine/Assets/FrameRetireQueue.hpp" ) )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string CodeOf( const std::string& path )
    {
        std::ifstream     in( path );
        std::stringstream buffer;
        buffer << in.rdbuf();
        return Desert::Tests::ConsumerText::StripCommentsAndLiterals( buffer.str() );
    }
} // namespace

// The two services that own built GPU objects eviction drops. Each must retire them through the queue and neither
// may idle the device to do it: a WaitDeviceIdle per sweep is a GPU stop per world cell left behind in a flight.
TEST( FrameRetireQueue, NeitherGpuServiceIdlesTheDeviceToReleaseWhatEvictionDropped )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the working directory";
    const std::string services = root + "Desert/Desert/Source/Engine/Runtime/Services/";
    for ( const char* service : { "Mesh/MeshService", "Material/MaterialService", "Texture/TextureService" } )
    {
        const std::string header = CodeOf( services + service + ".hpp" );
        const std::string source = CodeOf( services + service + ".cpp" );
        ASSERT_FALSE( header.empty() || source.empty() ) << service << " was not read";
        EXPECT_NE( header.find( "FrameRetireQueue" ), std::string::npos )
             << service << " no longer parks what it drops in a FrameRetireQueue";
        EXPECT_EQ( source.find( "WaitDeviceIdle" ), std::string::npos )
             << service << ".cpp idles the device again";
    }

    // WP14b: the texture service parks what it drops (not resets it), the engine sink forwards the drop to it, and
    // the frame loop collects its queue every frame - each link of the chain that frees a texture's image.
    const std::string texture = CodeOf( services + "Texture/TextureService.cpp" );
    EXPECT_NE( texture.find( "m_Retiring.Park(" ), std::string::npos ) << "TextureService drops without parking";
    const std::string sink = CodeOf( root + "Desert/Desert/Source/Engine/Assets/AssetEvictionServices.cpp" );
    EXPECT_NE( sink.find( "GetTextureService()->EvictBuilt(" ), std::string::npos )
         << "the engine sink no longer forwards a texture drop to TextureService";
    const std::string loop = CodeOf( root + "Desert/Desert/Source/Engine/Core/Application.cpp" );
    EXPECT_NE( loop.find( "GetTextureService()->RetireEvicted()" ), std::string::npos )
         << "the frame loop never collects the textures eviction parked, so none is ever freed";
}

TEST( EvictionDeadline, RequestsEveryFrameDoNotPostponeTheSweep )
{
    // World streaming asks on every tick that lets a cell go; a flight can do that every frame.
    Desert::Assets::EvictionDeadline deadline;
    std::string                      why;
    int                              sweeps          = 0;
    int                              firstSweepFrame = -1;
    for ( int frame = 0; frame < 12; ++frame )
    {
        deadline.Request( "cell left at frame " + std::to_string( frame ) );
        if ( deadline.Due( why ) )
        {
            if ( firstSweepFrame < 0 )
            {
                firstSweepFrame = frame;
                EXPECT_EQ( why, "cell left at frame 0" ) << "the first request's reason did not win";
            }
            ++sweeps;
        }
    }
    EXPECT_EQ( firstSweepFrame, Desert::Assets::EvictionDeadline::kFramesAfterFirstRequest - 1 )
         << "the sweep did not run two frames after the first request";
    EXPECT_GE( sweeps, 3 ) << "a request every frame starved the sweep";
}

TEST( EvictionDeadline, TwoRequestsInOneLoadAreOneSweepAndNothingIsDueWithoutARequest )
{
    Desert::Assets::EvictionDeadline deadline;
    std::string                      why;
    EXPECT_FALSE( deadline.Due( why ) );

    deadline.Request( "scene initialised" );
    EXPECT_FALSE( deadline.Due( why ) ); // frame 1: the second Init of the load lands here
    deadline.Request( "scene initialised again" );
    EXPECT_TRUE( deadline.Due( why ) );
    EXPECT_EQ( why, "scene initialised" );
    EXPECT_FALSE( deadline.Pending() );
    EXPECT_FALSE( deadline.Due( why ) );
}
