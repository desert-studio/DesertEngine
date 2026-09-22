#include <Common/Utilities/Lz4Block.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using Common::Utils::Lz4BlockBound;
using Common::Utils::Lz4BlockCompress;
using Common::Utils::Lz4BlockDecompress;

namespace
{
    // THE INPUT THE REFERENCE IMPLEMENTATION WAS GIVEN, rebuilt here rather than stored as bytes so
    // the two halves of the fixture cannot drift: if this function changes, the block below stops
    // decoding to it and the test says so. Three things in one buffer on purpose — long repeats (what
    // matches are for), incompressible noise (what literals are for), and a run of one byte (offset 1,
    // the overlapping copy that a plain block copy would get wrong).
    std::string FixtureSource()
    {
        std::string s;
        for ( int i = 0; i < 6; ++i )
            s += "DesertEngine pak fixture. ";
        for ( int i = 0; i < 256; ++i )
            s += static_cast<char>( i );
        s += "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        for ( int i = 0; i < 3; ++i )
            s += "DesertEngine pak fixture. ";
        for ( int i = 0; i < 200; ++i )
            s += static_cast<char>( ( i * 37 ) % 251 );
        return s;
    }

    // A block produced by the REFERENCE lz4 implementation (lz4 -1, block format, no frame) over
    // exactly the bytes FixtureSource() returns.
    // clang-format off
    const unsigned char kReferenceBlock[] = {
        0xFF, 0x0B, 0x44, 0x65, 0x73, 0x65, 0x72, 0x74, 0x45, 0x6E, 0x67, 0x69, 0x6E, 0x65, 0x20, 0x70,
        0x61, 0x6B, 0x20, 0x66, 0x69, 0x78, 0x74, 0x75, 0x72, 0x65, 0x2E, 0x20, 0x1A, 0x00, 0x6F, 0xFF,
        0xF4, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
        0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
        0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E,
        0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
        0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E,
        0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
        0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E,
        0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E,
        0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
        0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E,
        0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE,
        0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
        0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE,
        0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
        0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE,
        0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
        0xFF, 0x41, 0x41, 0x41, 0x03, 0x00, 0x0A, 0x0F, 0xA2, 0x01, 0x3B, 0xF0, 0xB9, 0x00, 0x25, 0x4A,
        0x6F, 0x94, 0xB9, 0xDE, 0x08, 0x2D, 0x52, 0x77, 0x9C, 0xC1, 0xE6, 0x10, 0x35, 0x5A, 0x7F, 0xA4,
        0xC9, 0xEE, 0x18, 0x3D, 0x62, 0x87, 0xAC, 0xD1, 0xF6, 0x20, 0x45, 0x6A, 0x8F, 0xB4, 0xD9, 0x03,
        0x28, 0x4D, 0x72, 0x97, 0xBC, 0xE1, 0x0B, 0x30, 0x55, 0x7A, 0x9F, 0xC4, 0xE9, 0x13, 0x38, 0x5D,
        0x82, 0xA7, 0xCC, 0xF1, 0x1B, 0x40, 0x65, 0x8A, 0xAF, 0xD4, 0xF9, 0x23, 0x48, 0x6D, 0x92, 0xB7,
        0xDC, 0x06, 0x2B, 0x50, 0x75, 0x9A, 0xBF, 0xE4, 0x0E, 0x33, 0x58, 0x7D, 0xA2, 0xC7, 0xEC, 0x16,
        0x3B, 0x60, 0x85, 0xAA, 0xCF, 0xF4, 0x1E, 0x43, 0x68, 0x8D, 0xB2, 0xD7, 0x01, 0x26, 0x4B, 0x70,
        0x95, 0xBA, 0xDF, 0x09, 0x2E, 0x53, 0x78, 0x9D, 0xC2, 0xE7, 0x11, 0x36, 0x5B, 0x80, 0xA5, 0xCA,
        0xEF, 0x19, 0x3E, 0x63, 0x88, 0xAD, 0xD2, 0xF7, 0x21, 0x46, 0x6B, 0x90, 0xB5, 0xDA, 0x04, 0x29,
        0x4E, 0x73, 0x98, 0xBD, 0xE2, 0x0C, 0x31, 0x56, 0x7B, 0xA0, 0xC5, 0xEA, 0x14, 0x39, 0x5E, 0x83,
        0xA8, 0xCD, 0xF2, 0x1C, 0x41, 0x66, 0x8B, 0xB0, 0xD5, 0xFA, 0x24, 0x49, 0x6E, 0x93, 0xB8, 0xDD,
        0x07, 0x2C, 0x51, 0x76, 0x9B, 0xC0, 0xE5, 0x0F, 0x34, 0x59, 0x7E, 0xA3, 0xC8, 0xED, 0x17, 0x3C,
        0x61, 0x86, 0xAB, 0xD0, 0xF5, 0x1F, 0x44, 0x69, 0x8E, 0xB3, 0xD8, 0x02, 0x27, 0x4C, 0x71, 0x96,
        0xBB, 0xE0, 0x0A, 0x2F, 0x54,
    };
    // clang-format on

    std::string Roundtrip( const std::string& input )
    {
        std::vector<char> packed( Lz4BlockBound( input.size() ) );
        const size_t      n = Lz4BlockCompress( input.data(), input.size(), packed.data(), packed.size() );
        EXPECT_GT( n, 0u );
        std::string out( input.size(), '\0' );
        EXPECT_TRUE( Lz4BlockDecompress( packed.data(), n, out.data(), out.size() ) );
        return out;
    }
} // namespace

// THE WITNESS THAT THIS IS A FORMAT AND NOT A PRIVATE SCHEME. Our compressor and our decompressor
// agreeing with each other proves nothing — two halves of one mistake agree perfectly. This decodes a
// block an INDEPENDENT implementation produced, which is the only evidence available that an archive
// written here can be read by anything else, and the only check that would catch a format drift
// introduced while tuning the encoder.
TEST( Lz4Block, DecodesABlockTheReferenceImplementationProduced )
{
    const std::string expected = FixtureSource();
    std::string       out( expected.size(), '\0' );
    ASSERT_TRUE( Lz4BlockDecompress( kReferenceBlock, sizeof( kReferenceBlock ), out.data(), out.size() ) );
    EXPECT_EQ( out, expected );
}

TEST( Lz4Block, RoundTripsTheFixtureAndItsCorners )
{
    const std::string fixture = FixtureSource();
    EXPECT_EQ( Roundtrip( fixture ), fixture );

    // The lengths where the format changes behaviour: below the 12-byte match limit nothing can be a
    // match at all, and the last 5 bytes are always literals.
    for ( size_t n = 1; n <= 40; ++n )
    {
        const std::string s( n, 'Z' );
        EXPECT_EQ( Roundtrip( s ), s ) << "length " << n;
    }

    // A run of one byte is an offset-1 match: the copy must see its OWN output as it goes, which is
    // the case a block copy silently gets wrong and which no amount of ordinary content exercises.
    const std::string run( 100000, 'Q' );
    EXPECT_EQ( Roundtrip( run ), run );
}

TEST( Lz4Block, IncompressibleInputStillRoundTrips )
{
    std::mt19937                            rng( 777 );
    std::uniform_int_distribution<unsigned> byte( 0, 255 );
    std::string                             noise( 200000, '\0' );
    for ( auto& c : noise )
        c = static_cast<char>( byte( rng ) );
    EXPECT_EQ( Roundtrip( noise ), noise );

    // And it does not pretend to have helped: the packer's threshold can only store such an entry
    // verbatim if the codec reports an honest size for it.
    std::vector<char> packed( Lz4BlockBound( noise.size() ) );
    const size_t      n = Lz4BlockCompress( noise.data(), noise.size(), packed.data(), packed.size() );
    EXPECT_GT( n, noise.size() * 9 / 10 );
}

// A decompressor that trusts its input is one that writes outside its buffer the day a check is
// skipped somewhere else. Every case here PARSES up to its last byte and then asks for something
// impossible — which is the shape a truncated download actually takes.
TEST( Lz4Block, MalformedBlocksAreRefusedRatherThanPartiallyDecoded )
{
    const std::string fixture = FixtureSource();
    std::vector<char> packed( Lz4BlockBound( fixture.size() ) );
    const size_t      n = Lz4BlockCompress( fixture.data(), fixture.size(), packed.data(), packed.size() );
    ASSERT_GT( n, 0u );
    std::string out( fixture.size(), '\0' );

    // Truncated at every length: never a crash, never a claim of success.
    for ( size_t cut = 1; cut < n; ++cut )
        EXPECT_FALSE( Lz4BlockDecompress( packed.data(), cut, out.data(), out.size() ) ) << "cut " << cut;

    // The right bytes, the wrong declared output size — both directions.
    std::string tooSmall( fixture.size() - 1, '\0' );
    EXPECT_FALSE( Lz4BlockDecompress( packed.data(), n, tooSmall.data(), tooSmall.size() ) );
    std::string tooLarge( fixture.size() + 1, '\0' );
    EXPECT_FALSE( Lz4BlockDecompress( packed.data(), n, tooLarge.data(), tooLarge.size() ) );

    // An offset that points before the start of the output is the classic read out of bounds: no
    // literals, then a 4-byte match at offset 1 with nothing written yet.
    const unsigned char backwards[] = { 0x00, 0x01, 0x00 };
    std::string         four( 4, '\0' );
    EXPECT_FALSE( Lz4BlockDecompress( backwards, sizeof( backwards ), four.data(), four.size() ) );

    // Offset zero is not a legal offset and must not be read as "copy from myself" — which would
    // hand the caller four bytes of whatever the buffer happened to hold.
    //
    // THE OUTPUT SIZE HERE IS LOAD-BEARING AND WAS WRONG ONCE. With a 4-byte output the sequence is
    // refused because the 4-byte match does not FIT after one literal, so removing the offset check
    // altogether left this assertion green — the test was pinning the length check and calling it the
    // offset check. Five bytes is exactly one literal plus the minimum match, so the only thing left
    // to refuse it is the offset being zero.
    const unsigned char zeroOffset[] = { 0x10, 'x', 0x00, 0x00 };
    std::string         five( 5, '\0' );
    EXPECT_FALSE( Lz4BlockDecompress( zeroOffset, sizeof( zeroOffset ), five.data(), five.size() ) );
}

TEST( Lz4Block, EmptyInputIsNotAnEntryThisCodecWillInvent )
{
    EXPECT_EQ( Lz4BlockCompress( "", 0, nullptr, 0 ), 0u );
    EXPECT_TRUE( Lz4BlockDecompress( "", 0, nullptr, 0 ) );
    std::string one( 1, '\0' );
    EXPECT_FALSE( Lz4BlockDecompress( "", 0, one.data(), one.size() ) );
}

// The output buffer belongs to the caller, and the codec must refuse rather than overrun it.
TEST( Lz4Block, CompressionRefusesWhenTheOutputWouldNotFit )
{
    const std::string fixture = FixtureSource();
    std::vector<char> tiny( 1 );
    EXPECT_EQ( Lz4BlockCompress( fixture.data(), fixture.size(), tiny.data(), tiny.size() ), 0u );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
