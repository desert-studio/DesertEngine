// THE TYPE HALF OF "A MEMCPY THROUGH A FAILED MAPPING MUST NOT COMPILE".
//
// `MappedMemoryCensus` proves the production code goes through the guard. This suite proves the guard is
// worth going through: that the pointer really cannot be extracted, that a refused mapping refuses every
// transfer, that an out-of-range transfer is refused with both numbers, and that the mapping releases
// itself exactly once however it is moved about.
//
// WHY THE "CANNOT" IS ASSERTED RATHER THAN INTENDED. A guarantee that lives only in a comment is one
// somebody removes by adding a convenience accessor, in good faith, six months from now — and the removal
// looks like an improvement in the diff. The detection idiom below turns "there is no way to get the
// bytes out" into a value a test can read, so the convenience accessor reddens this file the day it is
// written.
//
// It needs no Vulkan and no device: MappedMemory takes its unmap as a function pointer, which is also
// what lets the unmap-exactly-once property be observed at all.

#include <Engine/Graphic/MappedMemory.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

using Desert::Graphic::MappedMemory;

namespace
{
    int   g_Unmaps       = 0;
    void* g_LastUnmapped = nullptr;

    void CountingUnmap( void* allocation )
    {
        ++g_Unmaps;
        g_LastUnmapped = allocation;
    }

    struct Fixture
    {
        Fixture()
        {
            g_Unmaps       = 0;
            g_LastUnmapped = nullptr;
        }
    };

    // A handle standing in for a VmaAllocation; only its identity is ever used.
    int g_Allocation = 0;

    MappedMemory Live( uint8_t* bytes, std::size_t size )
    {
        return MappedMemory::Live( &g_Allocation, bytes, size, &CountingUnmap );
    }

    // ---- the detection idiom: "is this expression well-formed?" as a compile-time value -------------
    template <typename T, typename = void>
    struct HasData : std::false_type
    {
    };
    template <typename T>
    struct HasData<T, std::void_t<decltype( std::declval<T&>().Data() )>> : std::true_type
    {
    };

    template <typename T, typename = void>
    struct HasGetValue : std::false_type
    {
    };
    template <typename T>
    struct HasGetValue<T, std::void_t<decltype( std::declval<T&>().GetValue() )>> : std::true_type
    {
    };

    template <typename T, typename = void>
    struct HasDereference : std::false_type
    {
    };
    template <typename T>
    struct HasDereference<T, std::void_t<decltype( *std::declval<T&>() )>> : std::true_type
    {
    };

    template <typename T, typename = void>
    struct HasArrow : std::false_type
    {
    };
    template <typename T>
    struct HasArrow<T, std::void_t<decltype( std::declval<T&>().operator->() )>> : std::true_type
    {
    };

    // The exact expression the ten defective call sites were written as.
    template <typename T, typename = void>
    struct CanMemcpyInto : std::false_type
    {
    };
    template <typename T>
    struct CanMemcpyInto<T, std::void_t<decltype( std::memcpy( std::declval<T&>(), "", 0 ) )>> : std::true_type
    {
    };
} // namespace

TEST( MappedMemoryGuard, TheBytesCannotBeReachedWithoutTheAnswer )
{
    // THE WHOLE POINT OF THE TYPE, as five values rather than five sentences.
    EXPECT_FALSE( (std::is_convertible_v<MappedMemory, void*>))
         << "a conversion to void* is a memcpy waiting to be written";
    EXPECT_FALSE( (std::is_convertible_v<MappedMemory, uint8_t*>));
    EXPECT_FALSE( HasData<MappedMemory>::value ) << "Data() would hand the pointer straight back";
    EXPECT_FALSE( HasGetValue<MappedMemory>::value )
         << "GetValue() is the Ф4 shape: a failed result answering with a default-constructed T, which for "
            "a pointer is a null the caller cannot tell from a mapped one";
    EXPECT_FALSE( HasDereference<MappedMemory>::value );
    EXPECT_FALSE( HasArrow<MappedMemory>::value );
    EXPECT_FALSE( CanMemcpyInto<MappedMemory>::value )
         << "memcpy( mapping, src, n ) compiles again, which is the defect this type exists to make "
            "unwritable";

    // An implicit bool would find its way into memcpy through the pointer conversion; explicit does not.
    EXPECT_FALSE( (std::is_convertible_v<MappedMemory, bool>));
    EXPECT_TRUE( (std::is_constructible_v<bool, MappedMemory>));

    // Two owners would unmap twice.
    EXPECT_FALSE( std::is_copy_constructible_v<MappedMemory> );
    EXPECT_FALSE( std::is_copy_assignable_v<MappedMemory> );
    EXPECT_TRUE( std::is_move_constructible_v<MappedMemory> );
    EXPECT_TRUE( std::is_move_assignable_v<MappedMemory> );

    // The transfers answer, and the answer cannot be dropped silently.
    EXPECT_TRUE( (
         std::is_same_v<decltype( std::declval<MappedMemory&>().Write( nullptr, 0, 0 ) ), Common::BoolResultStr>));
    EXPECT_TRUE( (std::is_same_v<decltype( std::declval<const MappedMemory&>().ReadInto( nullptr, 0, 0 ) ),
                                 Common::BoolResultStr>));
    EXPECT_TRUE(
         (std::is_same_v<decltype( std::declval<MappedMemory&>().Fill( 0, 0, 0 ) ), Common::BoolResultStr>));
}

TEST( MappedMemoryGuard, ADefaultMappingIsARefusalAndNotAnEmptySuccess )
{
    // §1.4 of the contract: "not ready" and "nothing to do" must not be the same value.
    MappedMemory nothing;
    EXPECT_FALSE( nothing.IsMapped() );
    EXPECT_FALSE( static_cast<bool>( nothing ) );
    EXPECT_FALSE( nothing.GetRefusal().empty() );

    const uint32_t source = 0xDEADBEEFu;
    const auto     wrote  = nothing.Write( &source, sizeof( source ) );
    EXPECT_FALSE( wrote.IsSuccess() );
    EXPECT_NE( wrote.GetError().find( "never attempted" ), std::string::npos ) << wrote.GetError();
}

TEST( MappedMemoryGuard, ALiveMappingCarriesNoReasonAndAReleasedOneSaysSo )
{
    // The observable half of "a successful map allocates nothing": while the mapping is live it holds no
    // reason at all, because the standing reasons are `const char*` and the built one is empty. Assert
    // the states rather than the allocation — a malloc counter here would measure the allocator.
    Fixture                fixture;
    std::array<uint8_t, 4> device{};

    MappedMemory mapping = Live( device.data(), device.size() );
    EXPECT_TRUE( mapping.GetRefusal().empty() ) << "a live mapping has nothing to refuse";

    // A specific reason survives a stray Unmap; a generic one is replaced by the specific "released".
    MappedMemory refused = MappedMemory::Refused( "VK_ERROR_MEMORY_MAP_FAILED" );
    refused.Unmap();
    EXPECT_EQ( refused.GetRefusal(), "VK_ERROR_MEMORY_MAP_FAILED" )
         << "unmapping something that never mapped must not overwrite WHY it never mapped";

    mapping.Unmap();
    EXPECT_NE( mapping.GetRefusal().find( "already been released" ), std::string::npos ) << mapping.GetRefusal();
}

TEST( MappedMemoryGuard, ARefusalCarriesItsReasonIntoEveryTransfer )
{
    MappedMemory refused = MappedMemory::Refused( "vmaMapMemory failed: VK_ERROR_MEMORY_MAP_FAILED" );
    EXPECT_FALSE( refused.IsMapped() );

    uint8_t    scratch[4] = {};
    const auto read       = refused.ReadInto( scratch, sizeof( scratch ) );
    EXPECT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "VK_ERROR_MEMORY_MAP_FAILED" ), std::string::npos )
         << "the reason the mapping failed must survive to the caller, or the log says only that "
            "something did not work: "
         << read.GetError();

    // Nothing was touched.
    for ( uint8_t byte : scratch )
        EXPECT_EQ( byte, 0u );
}

TEST( MappedMemoryGuard, ASuccessfulMapReportingNoAddressIsStillARefusal )
{
    // The one shape the old code could not distinguish: VK_SUCCESS with a null out-pointer. It read as a
    // mapping, and the memcpy after it went to address zero.
    MappedMemory impossible = Live( nullptr, 64 );
    EXPECT_FALSE( impossible.IsMapped() );
    const uint8_t byte  = 1;
    const auto    wrote = impossible.Write( &byte, 1 );
    EXPECT_FALSE( wrote.IsSuccess() );
}

TEST( MappedMemoryGuard, WriteAndReadMoveTheBytesTheyPromise )
{
    Fixture                      fixture;
    std::array<uint8_t, 16>      device{};
    const std::array<uint8_t, 4> payload{ 1, 2, 3, 4 };

    MappedMemory mapping = Live( device.data(), device.size() );
    ASSERT_TRUE( mapping.IsMapped() );
    EXPECT_EQ( mapping.GetSize(), device.size() );

    const auto wrote = mapping.Write( payload.data(), payload.size(), 4 );
    ASSERT_TRUE( wrote.IsSuccess() ) << wrote.GetError();
    EXPECT_EQ( device[3], 0u );
    EXPECT_EQ( device[4], 1u );
    EXPECT_EQ( device[7], 4u );
    EXPECT_EQ( device[8], 0u );

    std::array<uint8_t, 4> back{};
    const auto             read = mapping.ReadInto( back.data(), back.size(), 4 );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( back, payload );

    const auto filled = mapping.Fill( 0xAB, 2, 14 );
    ASSERT_TRUE( filled.IsSuccess() ) << filled.GetError();
    EXPECT_EQ( device[13], 0u );
    EXPECT_EQ( device[14], 0xABu );
    EXPECT_EQ( device[15], 0xABu );
}

TEST( MappedMemoryGuard, ATransferThatDoesNotFitIsRefusedWithBothNumbers )
{
    // THE SECOND DEFECT THIS TYPE CLOSES. Every one of the old memcpy sites sized its copy from a width,
    // a height and a format computed several files away from the allocation it wrote into, and nothing
    // compared the two. VulkanVertexBuffer::SetData took a caller-supplied offset with no bound at all.
    Fixture                      fixture;
    std::array<uint8_t, 8>       device{};
    const std::array<uint8_t, 8> payload{ 9, 9, 9, 9, 9, 9, 9, 9 };

    MappedMemory mapping = Live( device.data(), device.size() );

    const auto tooBig = mapping.Write( payload.data(), payload.size(), 1 );
    EXPECT_FALSE( tooBig.IsSuccess() );
    EXPECT_NE( tooBig.GetError().find( "8" ), std::string::npos ) << tooBig.GetError();
    for ( uint8_t byte : device )
        EXPECT_EQ( byte, 0u ) << "a refused write must write NOTHING, not a prefix";

    // THE UNSIGNED WRAP, ASSERTED. `bytes > m_Size - offset` alone admits this one: 8 - 9 is a huge
    // number and every write then "fits". The offset is tested first for exactly that reason.
    const auto pastTheEnd = mapping.Write( payload.data(), 1, 9 );
    EXPECT_FALSE( pastTheEnd.IsSuccess() ) << "an offset past the end of the mapping must be refused";

    // The exact fit is not an off-by-one.
    const auto exact = mapping.Write( payload.data(), payload.size(), 0 );
    EXPECT_TRUE( exact.IsSuccess() ) << exact.GetError();

    // A zero-byte transfer at the very end is legal and does nothing.
    const auto empty = mapping.Write( payload.data(), 0, device.size() );
    EXPECT_TRUE( empty.IsSuccess() ) << empty.GetError();
}

TEST( MappedMemoryGuard, ANullSourceIsRefusedRatherThanCopied )
{
    Fixture                fixture;
    std::array<uint8_t, 4> device{};
    MappedMemory           mapping = Live( device.data(), device.size() );

    const auto wrote = mapping.Write( nullptr, 4 );
    EXPECT_FALSE( wrote.IsSuccess() );

    // memcpy from null is undefined even for zero bytes only in the strictest reading; zero bytes from
    // null is accepted here because the callers that pass it are passing an empty buffer, not a mistake.
    const auto nothing = mapping.Write( nullptr, 0 );
    EXPECT_TRUE( nothing.IsSuccess() ) << nothing.GetError();
}

TEST( MappedMemoryGuard, TheMappingReleasesItselfExactlyOnce )
{
    Fixture                fixture;
    std::array<uint8_t, 4> device{};

    {
        MappedMemory mapping = Live( device.data(), device.size() );
        EXPECT_EQ( g_Unmaps, 0 );
    }
    EXPECT_EQ( g_Unmaps, 1 ) << "the destructor is the unmap -- the thirteen hand-paired UnmapMemory calls "
                                "it replaces could each be skipped by an early return";
    EXPECT_EQ( g_LastUnmapped, &g_Allocation );
}

TEST( MappedMemoryGuard, MovingDoesNotUnmapTwiceAndLeavesTheSourceRefusing )
{
    Fixture                fixture;
    std::array<uint8_t, 4> device{};

    {
        MappedMemory source = Live( device.data(), device.size() );
        MappedMemory moved  = std::move( source );

        EXPECT_TRUE( moved.IsMapped() );
        EXPECT_FALSE( source.IsMapped() ); // NOLINT(bugprone-use-after-move) -- that IS the assertion
        const uint8_t byte  = 1;
        const auto    wrote = source.Write( &byte, 1 ); // NOLINT(bugprone-use-after-move)
        EXPECT_FALSE( wrote.IsSuccess() );
        EXPECT_EQ( g_Unmaps, 0 );
    }
    EXPECT_EQ( g_Unmaps, 1 );
}

TEST( MappedMemoryGuard, MoveAssignmentReleasesWhatItOverwrites )
{
    Fixture                fixture;
    std::array<uint8_t, 4> first{};
    std::array<uint8_t, 4> second{};
    int                    secondAllocation = 0;

    {
        MappedMemory mapping = Live( first.data(), first.size() );
        mapping = MappedMemory::Live( &secondAllocation, second.data(), second.size(), &CountingUnmap );

        EXPECT_EQ( g_Unmaps, 1 ) << "the mapping being replaced must be released, or it leaks silently";
        EXPECT_EQ( g_LastUnmapped, &g_Allocation );
    }
    EXPECT_EQ( g_Unmaps, 2 );
    EXPECT_EQ( g_LastUnmapped, &secondAllocation );
}

TEST( MappedMemoryGuard, UnmappingEarlyIsIdempotentAndClosesTheMapping )
{
    Fixture                fixture;
    std::array<uint8_t, 4> device{};

    MappedMemory mapping = Live( device.data(), device.size() );
    mapping.Unmap();
    mapping.Unmap();
    EXPECT_EQ( g_Unmaps, 1 );

    const uint8_t byte  = 1;
    const auto    wrote = mapping.Write( &byte, 1 );
    EXPECT_FALSE( wrote.IsSuccess() )
         << "a released mapping must refuse: the driver has taken the address back, and writing to it is "
            "the same corruption by a later route";
    EXPECT_NE( wrote.GetError().find( "already been released" ), std::string::npos ) << wrote.GetError();
}
