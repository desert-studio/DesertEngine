// The text asset header (AF6f): the second IAssetHeaderFormat. What is proven here is the property the
// format exists for - the header is read from a PREFIX of the file, so a body that is not even JSON does not
// stop it - and that the header is checked as strictly as the binary envelope's.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <gtest/gtest.h>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace Common::Content;

    constexpr uint32_t kTag = FourCC( "MATL" );

    const std::array<SubsystemVersion, 1> kKnown = { SubsystemVersion{ kTag, 1 } };

    AssetHeaderReadContext Context()
    {
        return AssetHeaderReadContext{ kKnown };
    }

    fs::path Write( const std::string& name, const std::string& text )
    {
        const fs::path dir = fs::temp_directory_path() / "DesertTextHeaderTest";
        fs::create_directories( dir );
        const fs::path file = dir / name;
        std::ofstream( file, std::ios::binary ) << text;
        return file;
    }

    std::string HeaderText( const AssetGuid& guid, uint32_t version = 1, const std::string& kind = "Material" )
    {
        TextAssetHeaderSerialized header = MakeTextHeader( ContentKind::Material, guid, kKnown );
        header.Kind                      = kind;
        header.Versions["MATL"]          = version;
        return rfl::json::write( header );
    }

    std::string Document( const std::string& header, const std::string& body )
    {
        return "{\n    \"Header\": " + header + ",\n" + body;
    }
} // namespace

TEST( TextAssetHeader, IsTheSecondRegisteredFormatAfterTheBinaryEnvelope )
{
    // The registry is pinned by NAME: a format added or dropped fails here with the list, not with a count
    // that can be edited to match.
    const std::vector<const IAssetHeaderFormat*> expected = {
         &BinaryEnvelopeHeaderFormat(), &TextHeaderFormat(), &MeshBinaryHeaderFormat() };
    const auto formats = AssetHeaderFormats();
    EXPECT_EQ( std::vector<const IAssetHeaderFormat*>( formats.begin(), formats.end() ), expected );
}

TEST( TextAssetHeader, ReadsTheHeaderOfAFileWhoseBodyIsNotJson )
{
    const AssetGuid   guid = AssetGuid::Generate();
    const std::string text =
         Document( HeaderText( guid ), "    \"Params\": [ {{{ this is not json, and never closes \"}" );
    const fs::path file = Write( "BrokenBody.demat", text );

    const auto header = ReadAssetHeader( file, Context() );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Kind, ContentKind::Material );
    EXPECT_EQ( header.GetValue().Guid, guid );
    ASSERT_EQ( header.GetValue().Subsystems.size(), 1u );
    EXPECT_EQ( header.GetValue().Subsystems[0], ( SubsystemVersion{ kTag, 1 } ) );
    EXPECT_TRUE( header.GetValue().Dependencies.empty() );

    // The full read of the same file refuses: the header was read without the body.
    EXPECT_FALSE( rfl::json::read<rfl::Generic>( text ).has_value() );
}

TEST( TextAssetHeader, StopsAtTheHeaderEvenWithBracesInsideStrings )
{
    std::istringstream in( "{ \"Header\": { \"Kind\": \"Mat}{erial\", \"Guid\": \"x\\\"}\" }, \"Body\": garbage" );
    const auto         object = ReadTextHeaderObject( in );
    ASSERT_TRUE( object ) << object.GetError();
    EXPECT_EQ( object.GetValue(), "{ \"Kind\": \"Mat}{erial\", \"Guid\": \"x\\\"}\" }" );
    std::string rest;
    std::getline( in, rest );
    EXPECT_EQ( rest, ", \"Body\": garbage" ) << "the reader went past the header's closing brace";
}

TEST( TextAssetHeader, GuidTextRoundTripsAndRefusesMalformedText )
{
    for ( int i = 0; i < 16; ++i )
    {
        const AssetGuid guid   = AssetGuid::Generate();
        const auto      parsed = AssetGuidFromText( AssetGuidToText( guid ) );
        ASSERT_TRUE( parsed ) << parsed.GetError();
        EXPECT_EQ( parsed.GetValue(), guid );
    }
    EXPECT_EQ( AssetGuidToText( AssetGuid{ 1, 0xABCDEF } ), "00000000000000010000000000abcdef" );
    EXPECT_FALSE( AssetGuidFromText( "0123" ) );
    EXPECT_FALSE( AssetGuidFromText( "0000000000000001000000000000ABCD" ) );
    EXPECT_FALSE( AssetGuidFromText( "000000000000000100000000000000g1" ) );
}

TEST( TextAssetHeader, TwoGeneratedGuidsDiffer )
{
    EXPECT_NE( AssetGuidToText( AssetGuid::Generate() ), AssetGuidToText( AssetGuid::Generate() ) );
}

TEST( TextAssetHeader, RefusesWhatTheBinaryEnvelopeRefuses )
{
    const AssetGuid guid = AssetGuid::Generate();
    const auto      read = [&]( const std::string& name, const std::string& header )
    { return ReadAssetHeader( Write( name, Document( header, "\"A\": 1\n}\n" ) ), Context() ); };

    EXPECT_FALSE( read( "Newer.demat", HeaderText( guid, 2 ) ) );
    EXPECT_FALSE( read( "Kind.demat", HeaderText( guid, 1, "NotAKind" ) ) );
    EXPECT_FALSE( read( "Null.demat", HeaderText( AssetGuid{} ) ) );

    TextAssetHeaderSerialized unknown = MakeTextHeader( ContentKind::Material, guid, kKnown );
    unknown.Versions["ZZZZ"]          = 1;
    EXPECT_FALSE( read( "Unknown.demat", rfl::json::write( unknown ) ) );

    TextAssetHeaderSerialized dependency = MakeTextHeader( ContentKind::Material, guid, kKnown );
    dependency.Dependencies.push_back( "not a guid" );
    EXPECT_FALSE( read( "Dependency.demat", rfl::json::write( dependency ) ) );
}

TEST( TextAssetHeader, AFileWhoseFirstMemberIsNotTheHeaderIsNotClaimed )
{
    const fs::path file   = Write( "NoHeader.demat", "{\n    \"Params\": [],\n    \"Header\": {}\n}\n" );
    const auto     header = ReadAssetHeader( file, Context() );
    ASSERT_FALSE( header );
    EXPECT_NE( header.GetError().find( "no header format recognises" ), std::string::npos ) << header.GetError();

    const fs::path truncated = Write( "Truncated.demat", "{\n    \"Header\": { \"Kind\": \"Material\"" );
    const auto     cut       = ReadAssetHeader( truncated, Context() );
    ASSERT_FALSE( cut );
    EXPECT_NE( cut.GetError().find( "ends inside the header" ), std::string::npos ) << cut.GetError();
}

// AF7: the registry cook RECORDS a header it cannot judge - an unknown tag, a newer version - and the
// build that loads the body refuses it. A malformed header is refused either way, and a file no format
// claims is "states no header", not an error.
TEST( TextAssetHeader, RecordOnlyKeepsVersionsItCannotJudgeAndStillRefusesAMalformedHeader )
{
    using namespace Common::Content;
    TextAssetHeaderSerialized header = MakeTextHeader( ContentKind::Material, AssetGuid::Generate(), {} );
    header.Versions["ZZZZ"]          = 9;
    const AssetHeaderReadContext judging{ {} };
    const AssetHeaderReadContext recording{ {}, true };
    EXPECT_FALSE( TextHeaderToAssetHeader( header, judging ) );
    const auto recorded = TextHeaderToAssetHeader( header, recording );
    ASSERT_TRUE( recorded ) << recorded.GetError();
    ASSERT_EQ( recorded.GetValue().Subsystems.size(), 1u );
    EXPECT_EQ( recorded.GetValue().Subsystems[0].Version, 9u );

    header.Guid = "00000000000000000000000000000000";
    EXPECT_FALSE( TextHeaderToAssetHeader( header, recording ) ) << "RecordOnly never admits a null GUID";

    const auto dir = std::filesystem::temp_directory_path() / "af7_record_only";
    std::filesystem::create_directories( dir );
    {
        std::ofstream( dir / "plain.dshader" ) << "Shader \"X\" {}";
    }
    const auto none = ReadAssetHeaderIfStated( dir / "plain.dshader", recording );
    ASSERT_TRUE( none ) << none.GetError();
    EXPECT_FALSE( none.GetValue().has_value() );
    EXPECT_FALSE( ReadAssetHeader( dir / "plain.dshader", recording ) );
    std::filesystem::remove_all( dir );
}
