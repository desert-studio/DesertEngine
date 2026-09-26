// The per-user driver pipeline cache (PSO1): path, device header, in-run persist schedule. No device.
#include <gtest/gtest.h>

#include <Engine/Assets/ContentGate.hpp>
#include <Engine/Graphic/PipelineBuilds.hpp>
#include <Engine/Graphic/PipelineCacheFile.hpp>

#include <thread>

using namespace Desert::Graphic::PipelineCacheFile;
using namespace std::chrono_literals;

namespace
{
    DeviceIdentity Gpu()
    {
        DeviceIdentity id{ 0x106b, 0x1a0, 0x00020001, {} };
        for ( size_t i = 0; i < id.CacheUuid.size(); ++i )
            id.CacheUuid[i] = static_cast<uint8_t>( i * 17 );
        return id;
    }
} // namespace

TEST( PipelineCacheFile, LivesInTheUsersDirectoryPerProject )
{
    const auto dir = Directory( "/home/u/.desertengine", "Sandbox" );
    EXPECT_EQ( dir, std::filesystem::path( "/home/u/.desertengine/PipelineCache/Sandbox" ) );
    EXPECT_NE( Directory( "/home/u/.desertengine", "Other" ), dir );
}

TEST( PipelineCacheFile, FileNameCarriesVendorDeviceDriverAndUuid )
{
    const DeviceIdentity id = Gpu();
    EXPECT_EQ( FileName( id ), "0000106b-000001a0-00020001-00112233445566778899aabbccddeeff.bin" );

    DeviceIdentity newDriver = id;
    newDriver.Driver += 1;
    EXPECT_NE( FileName( newDriver ), FileName( id ) ) << "a driver update must not overwrite the old blob";
    DeviceIdentity otherGpu = id;
    otherGpu.Device += 1;
    EXPECT_NE( FileName( otherGpu ), FileName( id ) );
    DeviceIdentity otherUuid = id;
    otherUuid.CacheUuid[15] ^= 1;
    EXPECT_NE( FileName( otherUuid ), FileName( id ) );
}

TEST( PipelineCacheFile, RoundTripsTheDriverBytes )
{
    const std::string blob( "\x01\x02\0driver-bytes", 15 );
    const auto        decoded = Decode( Gpu(), Encode( Gpu(), blob ) );
    ASSERT_TRUE( decoded.Blob ) << decoded.Refusal;
    EXPECT_EQ( *decoded.Blob, blob );
    EXPECT_TRUE( decoded.Refusal.empty() );
}

TEST( PipelineCacheFile, RefusesAnotherDevicesFileWithAReason )
{
    DeviceIdentity other = Gpu();
    other.Driver         = 7;
    const auto decoded   = Decode( Gpu(), Encode( other, "blob" ) );
    EXPECT_FALSE( decoded.Blob );
    EXPECT_NE( decoded.Refusal.find( "written for" ), std::string::npos ) << decoded.Refusal;
}

TEST( PipelineCacheFile, RefusesTornCorruptAndForeignFiles )
{
    const std::string file = Encode( Gpu(), "0123456789" );

    EXPECT_FALSE( Decode( Gpu(), file.substr( 0, file.size() - 1 ) ).Blob ) << "truncated payload";
    EXPECT_FALSE( Decode( Gpu(), file.substr( 0, 8 ) ).Blob ) << "truncated header";

    std::string flipped = file;
    flipped.back() ^= 0x20;
    const auto corrupt = Decode( Gpu(), flipped );
    EXPECT_FALSE( corrupt.Blob );
    EXPECT_NE( corrupt.Refusal.find( "hash" ), std::string::npos ) << corrupt.Refusal;

    std::string foreign = file;
    foreign[0]          = 'X';
    EXPECT_FALSE( Decode( Gpu(), foreign ).Blob ) << "a raw driver blob from the old layout is not ours";
}

TEST( PipelineCacheFile, PersistsDuringTheRunNotOnlyAtExit )
{
    PersistSchedule schedule( 2s );
    const auto      t0 = std::chrono::steady_clock::time_point{} + 100s;

    EXPECT_FALSE( schedule.Due( 0, t0 ) ) << "nothing built, nothing to write";
    EXPECT_TRUE( schedule.Due( 5, t0 ) ) << "the first write after start is not throttled";
    EXPECT_FALSE( schedule.Due( 9, t0 + 1s ) ) << "throttled inside the interval";
    EXPECT_TRUE( schedule.Due( 9, t0 + 2s ) ) << "due once the interval has passed and pipelines were built";
    EXPECT_FALSE( schedule.Due( 9, t0 + 60s ) ) << "no new pipelines, no rewrite";
    EXPECT_TRUE( schedule.Due( 10, t0 + 60s ) );
}

TEST( PipelineBuilds, CountsPendingAndStartedMonotonically )
{
    Desert::Graphic::PipelineBuilds::Tracker builds;
    builds.OnStarted();
    builds.OnStarted();
    EXPECT_EQ( builds.Pending(), 2u );
    builds.OnFinished();
    EXPECT_EQ( builds.Pending(), 1u );
    EXPECT_EQ( builds.Started(), 2u ) << "started never goes down: ContentGate reads it as 'asked this frame'";
}

TEST( PipelineBuilds, WaitIdleReturnsOnlyAfterTheLastCompileFinished )
{
    Desert::Graphic::PipelineBuilds::Tracker builds;
    builds.OnStarted();
    std::atomic<bool> finished{ false };
    std::thread       worker( [&] {
        std::this_thread::sleep_for( 50ms );
        finished = true;
        builds.OnFinished();
    } );
    builds.WaitIdle();
    EXPECT_TRUE( finished ) << "hot reload would replace a shader a compile is still reading";
    worker.join();
}

// The editor's splash and the runtime's loading screen: the gate is fed loader + pipelines (ContentWorkNow),
// so a pipeline still in the driver keeps the world Loading exactly like an asset read would.
TEST( PipelineBuilds, ContentGateWaitsForAPipelineStillInTheDriver )
{
    using Desert::Assets::ContentGate;
    using Desert::Assets::ContentState;
    Desert::Graphic::PipelineBuilds::Tracker builds;
    ContentGate                              gate( ContentState::Ready );
    const size_t                             assetsOutstanding = 0;
    const uint64_t                           assetsStarted     = 7;
    gate.BeginWorld( assetsStarted + builds.Started() );

    builds.OnStarted(); // the first frame asked for a material: its pipeline went to a worker
    for ( int frame = 0; frame < 10; ++frame )
        EXPECT_FALSE( gate.Tick( assetsOutstanding + builds.Pending(), assetsStarted + builds.Started() ) );
    EXPECT_TRUE( gate.Loading() ) << "opened while a pipeline was still compiling: that frame skips the mesh";

    builds.OnFinished();
    bool opened = false;
    for ( int frame = 0; frame < 3 && !opened; ++frame )
        opened = gate.Tick( assetsOutstanding + builds.Pending(), assetsStarted + builds.Started() );
    EXPECT_TRUE( opened );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
