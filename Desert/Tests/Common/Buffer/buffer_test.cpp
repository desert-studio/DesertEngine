#include <gtest/gtest.h>

#include <Common/Core/Memory/Buffer.hpp>

#include <cstring>

// WHY THIS SUITE EXISTS. Common::Memory::Buffer had NO test at all, and its Release() spent its whole
// life doing `delete[]` on a `void*` — an undefined array delete that only the workspace's `-w` kept
// quiet. The point of these cases is not that they would have failed on the old code (an undefined
// deallocation is free to look like it worked); it is that they put Buffer's allocate/write/release
// cycle in front of the ASan+UBSan job, which is the thing that can actually see a bad deallocation.

TEST( Buffer, AllocateThenReleaseRoundTrip )
{
    Common::Memory::Buffer buffer;
    EXPECT_FALSE( static_cast<bool>( buffer ) );
    EXPECT_EQ( buffer.GetSize(), 0u );
    EXPECT_EQ( buffer.GetAllocatedSize(), 0u );

    buffer.Allocate( 64 );
    EXPECT_TRUE( static_cast<bool>( buffer ) );
    EXPECT_EQ( buffer.GetSize(), 64u );
    EXPECT_EQ( buffer.GetAllocatedSize(), 64u );

    buffer.Release();
    EXPECT_FALSE( static_cast<bool>( buffer ) );
    EXPECT_EQ( buffer.GetSize(), 0u );
    EXPECT_EQ( buffer.GetAllocatedSize(), 0u );
}

// Release() is reached from Allocate() too, so re-allocating frees the previous block. This is the
// path that actually runs in the engine (VulkanVertexBuffer / VulkanIndexBuffer re-fill a Buffer).
TEST( Buffer, ReallocateReleasesThePreviousBlock )
{
    Common::Memory::Buffer buffer;
    buffer.Allocate( 16 );
    buffer.Allocate( 128 );
    EXPECT_EQ( buffer.GetAllocatedSize(), 128u );

    buffer.Release();
    // Releasing twice must be harmless: Release() nulls Data, and the guard is what makes the second
    // call a no-op rather than a double free.
    buffer.Release();
    EXPECT_EQ( buffer.GetAllocatedSize(), 0u );
}

TEST( Buffer, CopyOwnsItsOwnBytes )
{
    const char source[] = "desert";

    Common::Memory::Buffer buffer = Common::Memory::Buffer::Copy( source, sizeof( source ) );
    ASSERT_TRUE( static_cast<bool>( buffer ) );
    EXPECT_EQ( buffer.GetAllocatedSize(), sizeof( source ) );
    EXPECT_STREQ( buffer.As<const char>(), "desert" );

    buffer.Release();
}

TEST( Buffer, WriteThenReadBack )
{
    Common::Memory::Buffer buffer;
    buffer.Allocate( sizeof( std::uint32_t ) * 2 );
    buffer.ZeroInitialize();
    EXPECT_EQ( buffer.Read<std::uint32_t>( 0 ), 0u );

    const std::uint32_t value = 0xDEADBEEFu;
    buffer.Write( &value, sizeof( value ), 0 );
    EXPECT_EQ( buffer.Read<std::uint32_t>( 0 ), value );

    buffer.Release();
}
