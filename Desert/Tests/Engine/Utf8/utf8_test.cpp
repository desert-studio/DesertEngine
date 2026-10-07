#include <Engine/Text/Utf8.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace Desert::Text;

TEST( Utf8, DecodesAscii )
{
    EXPECT_EQ( Utf8Decode( "Hi!" ), ( std::vector<uint32_t>{ 'H', 'i', '!' } ) );
    EXPECT_TRUE( Utf8Decode( "" ).empty() );
}

TEST( Utf8, DecodesCyrillic )
{
    // The bug this exists for: byte-wise iteration turned "Привет" into 12 broken glyphs.
    const std::vector<uint32_t> cps = Utf8Decode( "Привет" );
    ASSERT_EQ( cps.size(), 6u );
    EXPECT_EQ( cps[0], 0x041Fu ); // П
    EXPECT_EQ( cps[5], 0x0442u ); // т
}

TEST( Utf8, DecodesThreeAndFourByteSequences )
{
    const std::vector<uint32_t> cps = Utf8Decode( "aé中\U0001F600" );
    ASSERT_EQ( cps.size(), 4u );
    EXPECT_EQ( cps[0], 0x0061u );
    EXPECT_EQ( cps[1], 0x00E9u );
    EXPECT_EQ( cps[2], 0x4E2Du );
    EXPECT_EQ( cps[3], 0x1F600u );
}

TEST( Utf8, MalformedInputYieldsReplacementAndKeepsGoing )
{
    // Every bad case must still make progress, or a decode loop would hang on it.
    const std::vector<uint32_t> stray = Utf8Decode( std::string( "a\x80"
                                                                 "b" ) );
    ASSERT_EQ( stray.size(), 3u );
    EXPECT_EQ( stray[1], kReplacementChar );
    EXPECT_EQ( stray[2], 'b' ); // resynced, not swallowed

    EXPECT_EQ( Utf8Decode( std::string( "a\xD0" ) ).back(), kReplacementChar );    // truncated
    EXPECT_EQ( Utf8Decode( std::string( "\xC0\xAF" ) )[0], kReplacementChar );     // overlong
    EXPECT_EQ( Utf8Decode( std::string( "\xED\xA0\x80" ) )[0], kReplacementChar ); // surrogate
}

TEST( Utf8, AppendRoundTrips )
{
    const std::string src = "Привет, 中 \U0001F600!";
    std::string       out;
    for ( uint32_t cp : Utf8Decode( src ) )
        Utf8Append( out, cp );
    EXPECT_EQ( out, src );
}

TEST( Utf8, PopBackRemovesAWholeCodepoint )
{
    std::string s = "Привет";
    Utf8PopBack( s );
    EXPECT_EQ( s, "Приве" ); // not half a character
    s = "a";
    Utf8PopBack( s );
    EXPECT_TRUE( s.empty() );
    Utf8PopBack( s ); // empty input must not underflow
    EXPECT_TRUE( s.empty() );
}
