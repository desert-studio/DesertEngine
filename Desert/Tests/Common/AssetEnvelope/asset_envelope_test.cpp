// The asset envelope's contract: kind comes from the header and only from the header, the header is
// readable without the body, a read-then-write reproduces the file byte for byte, and every way a file
// can be wrong is refused with a message that names what is wrong.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Utilities/Crc32c.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace Common::Content;

    constexpr uint32_t MESH_TAG     = FourCC( "MESH" );
    constexpr uint32_t MATSLOTS_TAG = FourCC( "MSLT" );

    const std::array<SubsystemVersion, 2> KNOWN = { { { MESH_TAG, 3 }, { MATSLOTS_TAG, 1 } } };

    AssetHeaderReadContext Context()
    {
        return { KNOWN };
    }

    std::vector<std::byte> Bytes( std::string_view text )
    {
        const auto view = std::as_bytes( std::span( text.data(), text.size() ) );
        return { view.begin(), view.end() };
    }

    AssetEnvelope SampleEnvelope( ContentKind kind = ContentKind::StaticMesh )
    {
        AssetEnvelope envelope;
        envelope.Asset.Kind         = kind;
        envelope.Asset.Guid         = { 0x0123456789abcdefull, 0xfedcba9876543210ull };
        envelope.Asset.Subsystems   = { { MESH_TAG, 3 }, { MATSLOTS_TAG, 1 } };
        envelope.Asset.Dependencies = { { 1, 2 }, { 3, 4 } };

        EnvelopeMeta meta;
        meta.Name   = "SM_Rock";
        meta.Tags   = { "rock", "desert" };
        meta.Bounds = EnvelopeBounds{ { -50.0f, -50.0f, 0.0f }, { 50.0f, 50.0f, 120.5f } };
        envelope.Sections.push_back(
             { EnvelopeSection::Meta, EnvelopeCodec::Stored, EncodeEnvelopeMeta( meta ) } );
        envelope.Sections.push_back( { EnvelopeSection::ImportInfo, EnvelopeCodec::Stored, Bytes( "fbx 2020" ) } );
        envelope.Sections.push_back( { EnvelopeSection::Source, EnvelopeCodec::Stored, Bytes( "source bytes" ) } );
        envelope.Sections.push_back( { EnvelopeSection::Payload, EnvelopeCodec::Stored,
                                       Bytes( "payload bytes in the kind's own codec" ) } );
        return envelope;
    }

    std::vector<std::byte> Written( const AssetEnvelope& envelope )
    {
        auto bytes = WriteAssetEnvelope( envelope );
        EXPECT_TRUE( bytes ) << bytes.GetError();
        return bytes ? bytes.GetValue() : std::vector<std::byte>{};
    }

    uint32_t Load32( const std::vector<std::byte>& bytes, std::size_t at )
    {
        uint32_t v = 0;
        for ( std::size_t i = 0; i < 4; ++i )
            v |= static_cast<uint32_t>( std::to_integer<uint8_t>( bytes[at + i] ) ) << ( 8 * i );
        return v;
    }

    void Store32( std::vector<std::byte>& bytes, std::size_t at, uint32_t v )
    {
        for ( std::size_t i = 0; i < 4; ++i )
            bytes[at + i] = static_cast<std::byte>( v >> ( 8 * i ) );
    }

    // Re-stamps the header CRC after a deliberate edit, so the test reaches the check BEHIND the CRC.
    void FixCrc( std::vector<std::byte>& bytes )
    {
        const uint32_t headerSize = Load32( bytes, 8 );
        Store32( bytes, 12, 0 );
        Store32( bytes, 12, Common::Utils::Crc32c( bytes.data(), headerSize ) );
    }

    // Offset of the first TOC entry: the TOC is the last thing in the header.
    std::size_t TocStart( const std::vector<std::byte>& bytes, std::size_t sections )
    {
        return Load32( bytes, 8 ) - sections * 32;
    }

    fs::path ScratchDir()
    {
        const AssetGuid unique = AssetGuid::Generate();
        const fs::path  dir = fs::temp_directory_path() / ( "AssetEnvelopeTest-" + std::to_string( unique.Lo ) );
        fs::create_directories( dir );
        return dir;
    }

    void WriteRaw( const fs::path& file, const std::vector<std::byte>& bytes )
    {
        std::ofstream out( file, std::ios::binary | std::ios::trunc );
        out.write( reinterpret_cast<const char*>( bytes.data() ), static_cast<std::streamsize>( bytes.size() ) );
    }

    std::string ErrorOf( const auto& result )
    {
        EXPECT_FALSE( result ) << "expected a refusal";
        return result ? std::string() : result.GetError();
    }

    bool Mentions( const std::string& error, std::string_view what )
    {
        return error.find( what ) != std::string::npos;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------------------------------

TEST( AssetEnvelope, ReadThenWriteReproducesTheFileByteForByte )
{
    const AssetEnvelope source = SampleEnvelope();
    const auto          first  = Written( source );

    auto read = ReadAssetEnvelope( first, Context() );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( read.GetValue().Asset, source.Asset );
    EXPECT_EQ( read.GetValue().Sections, source.Sections );
    EXPECT_EQ( Written( read.GetValue() ), first );

    const fs::path file = ScratchDir() / "SM_Rock.stmesh";
    ASSERT_TRUE( WriteAssetEnvelopeFile( file, source ) );
    auto fromDisk = ReadAssetEnvelopeFile( file, Context() );
    ASSERT_TRUE( fromDisk ) << fromDisk.GetError();
    EXPECT_EQ( Written( fromDisk.GetValue() ), first );
    fs::remove_all( file.parent_path() );
}

TEST( AssetEnvelope, MetaSectionRoundTripsAndRefusesTruncation )
{
    EnvelopeMeta meta;
    meta.Name = "T_Sand";
    meta.Tags = { "terrain" };
    auto back = DecodeEnvelopeMeta( EncodeEnvelopeMeta( meta ) );
    ASSERT_TRUE( back ) << back.GetError();
    EXPECT_EQ( back.GetValue(), meta );

    meta.Bounds  = EnvelopeBounds{ { 1.0f, 2.0f, 3.0f }, { 4.0f, 5.0f, 6.0f } };
    auto encoded = EncodeEnvelopeMeta( meta );
    auto bounded = DecodeEnvelopeMeta( encoded );
    ASSERT_TRUE( bounded ) << bounded.GetError();
    EXPECT_EQ( bounded.GetValue(), meta );

    encoded.pop_back();
    EXPECT_TRUE( Mentions( ErrorOf( DecodeEnvelopeMeta( encoded ) ), "meta bounds" ) );
}

TEST( AssetEnvelope, GeneratedGuidsAreNonNullAndDistinct )
{
    const AssetGuid a = AssetGuid::Generate();
    const AssetGuid b = AssetGuid::Generate();
    EXPECT_FALSE( a.IsNull() );
    EXPECT_NE( a, b );
}

// ---------------------------------------------------------------------------------------------------
// Kind comes from the header, never from the name
// ---------------------------------------------------------------------------------------------------

// For every kind in the census: an envelope of that kind, saved under that kind's extension, reads back
// as exactly the kind that was written, and that kind is one of those the census files under the
// extension. An extension may be shared by several kinds (Texture and Skybox are both .detex): the name
// is for people, the header is what decides, so the gate is "header kind is in the extension's set",
// not "the extension names one kind". The extension is resolved HERE, by the test, from the census; the
// reader is never given it.
TEST( AssetEnvelope, KindInHeaderEqualsKindByExtensionForEveryKind )
{
    const auto                                           kinds = ContentKinds();
    std::map<std::string_view, std::vector<ContentKind>> byExtension;
    for ( std::size_t i = 0; i < kinds.size(); ++i )
    {
        ASSERT_FALSE( kinds[i].Extension.empty() ) << kinds[i].Name;
        byExtension[kinds[i].Extension].push_back( static_cast<ContentKind>( i ) );
    }

    const fs::path dir = ScratchDir();
    for ( std::size_t i = 0; i < kinds.size(); ++i )
    {
        const auto     kind = static_cast<ContentKind>( i );
        const fs::path file = dir / ( std::string( "Asset" ) + std::string( kinds[i].Extension ) );
        ASSERT_TRUE( WriteAssetEnvelopeFile( file, SampleEnvelope( kind ) ) );

        auto header = ReadAssetHeader( file, Context() );
        ASSERT_TRUE( header ) << kinds[i].Name << ": " << header.GetError();
        EXPECT_EQ( header.GetValue().Kind, kind ) << kinds[i].Name;
        const auto& shared = byExtension.at( file.extension().string() );
        EXPECT_NE( std::find( shared.begin(), shared.end(), header.GetValue().Kind ), shared.end() )
             << kinds[i].Name << " read back as a kind the census does not file under " << kinds[i].Extension;
        EXPECT_EQ( KindName( header.GetValue().Kind ), kinds[i].Name );
    }
    fs::remove_all( dir );
}

// A renamed file keeps its kind, a file with no extension still has one, and a non-envelope wearing an
// envelope's extension is refused: the decoder is picked by content.
TEST( AssetEnvelope, DecoderIsChosenByContentNotByName )
{
    const fs::path dir = ScratchDir();
    for ( const char* name : { "Material.tex", "NoExtension", "Material.demat" } )
    {
        ASSERT_TRUE( WriteAssetEnvelopeFile( dir / name, SampleEnvelope( ContentKind::Material ) ) );
        auto header = ReadAssetHeader( dir / name, Context() );
        ASSERT_TRUE( header ) << name << ": " << header.GetError();
        EXPECT_EQ( header.GetValue().Kind, ContentKind::Material ) << name;
    }

    WriteRaw( dir / "Impostor.stmesh", Bytes( "not an envelope, whatever the name says" ) );
    EXPECT_TRUE(
         Mentions( ErrorOf( ReadAssetHeader( dir / "Impostor.stmesh", Context() ) ), "no header format" ) );
    fs::remove_all( dir );
}

// The source-text half of the same gate: the envelope's reader cannot consult a file's name for a
// decision it is not given the name for, and must not start to.
TEST( AssetEnvelope, ReaderSourceNeverConsultsAnExtension )
{
    fs::path root = fs::current_path();
    while ( !root.empty() && !fs::exists( root / "Desert/Common/Source/Common/Content/AssetEnvelope.cpp" ) )
    {
        if ( root == root.parent_path() )
        {
            root.clear();
            break;
        }
        root = root.parent_path();
    }
    ASSERT_FALSE( root.empty() ) << "could not locate the repository from " << fs::current_path();

    for ( const char* relative : { "Desert/Common/Source/Common/Content/AssetEnvelope.cpp",
                                   "Desert/Common/Source/Common/Content/AssetEnvelope.hpp" } )
    {
        std::ifstream      in( root / relative );
        std::ostringstream text;
        text << in.rdbuf();
        const std::string source = text.str();
        ASSERT_FALSE( source.empty() ) << relative;
        for ( std::string_view token : { "extension()", ".Extension", "KindSpec(" } )
            EXPECT_FALSE( Mentions( source, token ) ) << relative << " uses " << token;
    }
}

// ---------------------------------------------------------------------------------------------------
// The header is read without the body
// ---------------------------------------------------------------------------------------------------

TEST( AssetEnvelope, HeaderReadsFromThePrefixAloneWithNoBodyPresent )
{
    const auto bytes = Written( SampleEnvelope() );
    auto       full  = ReadEnvelopeHeader( bytes, Context() );
    ASSERT_TRUE( full ) << full.GetError();
    const uint32_t headerSize = full.GetValue().HeaderSize;
    ASSERT_LT( headerSize, bytes.size() );

    // The buffer ENDS where the header ends: there is no body to read, so the answer cannot depend on it.
    auto prefixOnly = ReadEnvelopeHeader( std::span( bytes ).first( headerSize ), Context() );
    ASSERT_TRUE( prefixOnly ) << prefixOnly.GetError();
    EXPECT_EQ( prefixOnly.GetValue().Asset, full.GetValue().Asset );
    EXPECT_EQ( prefixOnly.GetValue().Toc, full.GetValue().Toc );

    // Same through a stream holding only the prefix.
    std::istringstream stream( std::string( reinterpret_cast<const char*>( bytes.data() ), headerSize ) );
    auto               streamed = ReadEnvelopeHeader( stream, Context() );
    ASSERT_TRUE( streamed ) << streamed.GetError();
    EXPECT_EQ( streamed.GetValue().Asset, full.GetValue().Asset );
}

TEST( AssetEnvelope, CorruptOrTruncatedBodyLeavesTheHeaderReadableButTheFileRefused )
{
    const auto     bytes = Written( SampleEnvelope() );
    const fs::path dir   = ScratchDir();

    auto corrupt = bytes;
    corrupt.back() ^= std::byte{ 0x5a };
    WriteRaw( dir / "Corrupt.stmesh", corrupt );
    auto header = ReadAssetHeader( dir / "Corrupt.stmesh", Context() );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Kind, ContentKind::StaticMesh );
    EXPECT_TRUE( Mentions( ErrorOf( ReadAssetEnvelopeFile( dir / "Corrupt.stmesh", Context() ) ), "hash" ) );

    auto truncated = bytes;
    truncated.resize( bytes.size() - 5 );
    WriteRaw( dir / "Truncated.stmesh", truncated );
    auto cut = ReadAssetHeader( dir / "Truncated.stmesh", Context() );
    ASSERT_TRUE( cut ) << cut.GetError();
    EXPECT_EQ( cut.GetValue().Guid, SampleEnvelope().Asset.Guid );
    EXPECT_TRUE( Mentions( ErrorOf( ReadAssetEnvelopeFile( dir / "Truncated.stmesh", Context() ) ),
                           "ends past the file size" ) );
    fs::remove_all( dir );
}

// ---------------------------------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------------------------------

TEST( AssetEnvelope, RefusesAnUnknownKind )
{
    auto bytes = Written( SampleEnvelope( ContentKind::Texture ) );
    // Kind name sits at 32 as u16 length + text; "Texture" -> "Tixture", same length.
    ASSERT_EQ( std::to_integer<char>( bytes[35] ), 'e' );
    bytes[35] = std::byte{ 'i' };
    FixCrc( bytes );
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( bytes, Context() ) ), "unknown kind 'Tixture'" ) );
}

TEST( AssetEnvelope, RefusesAHeaderWhoseCrcDoesNotMatch )
{
    auto bytes = Written( SampleEnvelope() );
    bytes[20] ^= std::byte{ 0x01 }; // inside the GUID
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( bytes, Context() ) ), "CRC-32C" ) );
}

TEST( AssetEnvelope, RefusesBadMagicAndNewerContainer )
{
    auto magic = Written( SampleEnvelope() );
    magic[0]   = std::byte{ 'X' };
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( magic, Context() ) ), "magic" ) );

    auto newer = Written( SampleEnvelope() );
    Store32( newer, 4, ASSET_ENVELOPE_CONTAINER_VERSION + 1 );
    FixCrc( newer );
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( newer, Context() ) ), "container version" ) );
}

TEST( AssetEnvelope, RefusesABrokenToc )
{
    const std::size_t sections = SampleEnvelope().Sections.size();

    auto       shifted = Written( SampleEnvelope() );
    const auto offset0 = TocStart( shifted, sections ) + 8;
    Store32( shifted, offset0, Load32( shifted, offset0 ) + 1 );
    FixCrc( shifted );
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( shifted, Context() ) ), "starts at" ) );

    auto unknownTag = Written( SampleEnvelope() );
    Store32( unknownTag, TocStart( unknownTag, sections ), FourCC( "JUNK" ) );
    FixCrc( unknownTag );
    EXPECT_TRUE(
         Mentions( ErrorOf( ReadEnvelopeHeader( unknownTag, Context() ) ), "unknown section tag 'JUNK'" ) );

    auto duplicate = Written( SampleEnvelope() );
    Store32( duplicate, TocStart( duplicate, sections ) + 32, static_cast<uint32_t>( EnvelopeSection::Meta ) );
    FixCrc( duplicate );
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( duplicate, Context() ) ), "listed twice" ) );

    auto codec = Written( SampleEnvelope() );
    Store32( codec, TocStart( codec, sections ) + 4, 7 );
    FixCrc( codec );
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( codec, Context() ) ), "unknown codec 7" ) );

    // A section count that promises more TOC than the header holds.
    auto count = Written( SampleEnvelope() );
    Store32( count, TocStart( count, sections ) - 4, 1000 );
    FixCrc( count );
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( count, Context() ) ), "section TOC" ) );
}

TEST( AssetEnvelope, RefusesASectionPastTheEndOfTheFileAndTrailingBytes )
{
    auto bytes = Written( SampleEnvelope() );
    bytes.pop_back();
    EXPECT_TRUE( Mentions( ErrorOf( ReadAssetEnvelope( bytes, Context() ) ), "ends past the file size" ) );

    auto trailing = Written( SampleEnvelope() );
    trailing.push_back( std::byte{ 0 } );
    EXPECT_TRUE( Mentions( ErrorOf( ReadAssetEnvelope( trailing, Context() ) ), "trailing bytes" ) );
}

TEST( AssetEnvelope, RefusesASubsystemNewerOrUnknownToThisBuild )
{
    const auto bytes = Written( SampleEnvelope() );

    const std::array<SubsystemVersion, 2> older = { { { MESH_TAG, 2 }, { MATSLOTS_TAG, 1 } } };
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( bytes, AssetHeaderReadContext{ older } ) ),
                           "subsystem 'MESH' version 3 is newer than this build's 2" ) );

    const std::array<SubsystemVersion, 1> partial = { { { MESH_TAG, 3 } } };
    EXPECT_TRUE( Mentions( ErrorOf( ReadEnvelopeHeader( bytes, AssetHeaderReadContext{ partial } ) ),
                           "subsystem 'MSLT' (version 1) is unknown" ) );

    // Older-than-known is readable: that is what the version is for.
    const std::array<SubsystemVersion, 2> newer = { { { MESH_TAG, 9 }, { MATSLOTS_TAG, 1 } } };
    EXPECT_TRUE( ReadEnvelopeHeader( bytes, AssetHeaderReadContext{ newer } ) );
}

TEST( AssetEnvelope, WriterRefusesWhatTheReaderWouldRefuse )
{
    auto nullGuid       = SampleEnvelope();
    nullGuid.Asset.Guid = {};
    EXPECT_TRUE( Mentions( ErrorOf( WriteAssetEnvelope( nullGuid ) ), "null GUID" ) );

    auto duplicate = SampleEnvelope();
    duplicate.Sections.push_back( duplicate.Sections.front() );
    EXPECT_TRUE( Mentions( ErrorOf( WriteAssetEnvelope( duplicate ) ), "section 'META' listed twice" ) );

    auto subsystem = SampleEnvelope();
    subsystem.Asset.Subsystems.push_back( { MESH_TAG, 1 } );
    EXPECT_TRUE( Mentions( ErrorOf( WriteAssetEnvelope( subsystem ) ), "subsystem 'MESH' listed twice" ) );

    auto noKind       = SampleEnvelope();
    noKind.Asset.Kind = ContentKind::COUNT;
    EXPECT_TRUE( Mentions( ErrorOf( WriteAssetEnvelope( noKind ) ), "kind is COUNT" ) );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
