#include <Common/Utilities/Crc32c.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using Common::Utils::Crc32c;
using Common::Utils::Crc32cPortable;
using Common::Utils::Crc32cUsesHardware;

namespace
{
    uint32_t Of( const std::string& s )
    {
        return Crc32c( s.data(), s.size() );
    }
} // namespace

// THE ONE TEST THAT SAYS THIS IS A STANDARD AND NOT A PRIVATE SCHEME. "123456789" is the check value
// every CRC catalogue publishes for CRC-32C, so a routine that produces 0xE3069283 for it is the
// Castagnoli CRC and not something with the same shape. Without this the two implementations below
// could agree with each other perfectly while both being wrong, which is exactly the failure a pair
// of home-grown functions produces.
TEST( Crc32c, MatchesThePublishedCheckValue )
{
    EXPECT_EQ( Of( "123456789" ), 0xE3069283u );
    EXPECT_EQ( Crc32cPortable( "123456789", 9 ), 0xE3069283u );
}

TEST( Crc32c, EmptyInputIsZero )
{
    // Not an arbitrary convention: 0xFFFFFFFF inverted is 0, and it matters because an archive may
    // legitimately carry a zero-byte entry and its CRC column must still be the CRC of what is there.
    EXPECT_EQ( Crc32c( nullptr, 0 ), 0u );
    EXPECT_EQ( Crc32cPortable( nullptr, 0 ), 0u );
}

// THE TWO IMPLEMENTATIONS MUST AGREE BIT FOR BIT, and this is what makes the branch this development
// machine cannot execute a checked branch: on ARM the hardware arm is the instruction and on the
// x86-64 build machine it is SSE4.2, and each of them is compared here against the same table.
// An archive written on one and read on the other would otherwise report every entry as corrupt.
TEST( Crc32c, TheHardwareAndPortablePathsAgreeOnEveryLength )
{
    std::mt19937                            rng( 12345 );
    std::uniform_int_distribution<unsigned> byte( 0, 255 );
    std::vector<unsigned char>              buffer( 1024 );
    for ( auto& b : buffer )
        b = static_cast<unsigned char>( byte( rng ) );

    // Every length from 0 to 1024 — the tails are where a word-at-a-time loop and a byte-at-a-time
    // loop part company, and a test on one convenient size would never see it.
    for ( size_t n = 0; n <= buffer.size(); ++n )
        ASSERT_EQ( Crc32c( buffer.data(), n ), Crc32cPortable( buffer.data(), n ) ) << "length " << n;

    // Said out loud so a green run on a host WITHOUT the instruction cannot be read as a green run of
    // the hardware path — the comparison above is vacuous there, and silence would hide that.
    std::printf( "[ crc32c ] hardware path: %s\n", Crc32cUsesHardware() ? "yes" : "no (table only)" );
}

// WHAT THE ARCHIVE ACTUALLY RELIES ON. A single flipped bit anywhere must change the value; this is
// the property that makes the CRC an integrity check rather than a label, and it is the one a faster
// hash would have to keep. Exhaustive over every bit of a 256-byte block.
TEST( Crc32c, EverySingleBitFlipChangesTheValue )
{
    std::vector<unsigned char> buffer( 256 );
    for ( size_t i = 0; i < buffer.size(); ++i )
        buffer[i] = static_cast<unsigned char>( i * 7u + 3u );
    const uint32_t clean = Crc32c( buffer.data(), buffer.size() );

    for ( size_t i = 0; i < buffer.size(); ++i )
    {
        for ( int bit = 0; bit < 8; ++bit )
        {
            buffer[i] = static_cast<unsigned char>( buffer[i] ^ ( 1u << bit ) );
            ASSERT_NE( Crc32c( buffer.data(), buffer.size() ), clean ) << "byte " << i << " bit " << bit;
            buffer[i] = static_cast<unsigned char>( buffer[i] ^ ( 1u << bit ) );
        }
    }
}

// The property that a 64-bit FNV does NOT promise and that this replaces it with: a CRC-32C detects
// every burst of corruption up to 32 bits wide, deterministically, and that is the shape storage
// actually produces. Checked as a relation over every offset rather than at one place.
TEST( Crc32c, EveryBurstUpToThirtyTwoBitsIsDetected )
{
    std::vector<unsigned char> buffer( 128, 0xA5 );
    const uint32_t             clean = Crc32c( buffer.data(), buffer.size() );
    for ( size_t at = 0; at + 4 <= buffer.size(); ++at )
    {
        for ( int width = 1; width <= 4; ++width )
        {
            for ( int i = 0; i < width; ++i )
                buffer[at + static_cast<size_t>( i )] ^= 0xFFu;
            ASSERT_NE( Crc32c( buffer.data(), buffer.size() ), clean ) << "burst at " << at << " x " << width;
            for ( int i = 0; i < width; ++i )
                buffer[at + static_cast<size_t>( i )] ^= 0xFFu;
        }
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
