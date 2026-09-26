// The per-user driver pipeline cache (PSO1): path, device header, in-run persist schedule. No device.
#include <gtest/gtest.h>

#include <Engine/Graphic/PipelineCacheFile.hpp>

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

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
