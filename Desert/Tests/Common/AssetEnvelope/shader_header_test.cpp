// The shader comment header (T7j): the text header object on a .shader's first line, behind `// DesertAsset `.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ShaderAssetHeader.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace
{
    namespace fs = std::filesystem;
    using namespace Common::Content;

    constexpr uint32_t                    kShdr   = FourCC( "SHDR" );
    const std::array<SubsystemVersion, 1> kKnown  = { SubsystemVersion{ kShdr, 1 } };
    const std::string                     kSource = "Shader \"Probe\"\n{\n    // a body comment\n}\n";

    fs::path Write( const std::string& name, const std::string& text )
    {
        const fs::path dir = fs::temp_directory_path() / "DesertShaderHeaderTest";
        fs::create_directories( dir );
        const fs::path file = dir / name;
        std::ofstream( file, std::ios::binary ) << text;
        return file;
    }
} // namespace

TEST( ShaderCommentHeader, TheLineRoundTripsByteForByteAndTheFileReadsAsAShaderWithItsGuid )
{
    const AssetGuid   guid = AssetGuid::Generate();
    const std::string line = WriteShaderHeaderLine( MakeTextHeader( ContentKind::Shader, guid, kKnown ) );
    ASSERT_TRUE( line.starts_with( kShaderHeaderPrefix ) );
    ASSERT_EQ( line.back(), '\n' );

    const auto parsed = ReadShaderHeader( line + kSource );
    ASSERT_FALSE( !parsed ) << parsed.GetError();
    EXPECT_EQ( WriteShaderHeaderLine( parsed.GetValue() ), line );

    const auto header =
         ReadAssetHeader( Write( "Headed.shader", line + kSource ), AssetHeaderReadContext{ kKnown } );
    ASSERT_FALSE( !header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Kind, ContentKind::Shader );
    EXPECT_EQ( header.GetValue().Guid, guid );
    ASSERT_EQ( header.GetValue().Subsystems.size(), 1u );
    EXPECT_EQ( header.GetValue().Subsystems[0].Tag, kShdr );
    EXPECT_EQ( header.GetValue().Subsystems[0].Version, 1u );
}

TEST( ShaderCommentHeader, ACarriageReturnBeforeTheNewlineIsNotPartOfTheObject )
{
    const AssetGuid guid = AssetGuid::Generate();
    std::string     line = WriteShaderHeaderLine( MakeTextHeader( ContentKind::Shader, guid, kKnown ) );
    line.insert( line.size() - 1, "\r" );
    const auto parsed = ReadShaderHeader( line + kSource );
    ASSERT_FALSE( !parsed ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue().Guid, AssetGuidToText( guid ) );
}

TEST( ShaderCommentHeader, AHeaderlessShaderIsClaimedByNoFormatAndRefusedByTheReaderNamingThePrefix )
{
    const auto stated =
         ReadAssetHeaderIfStated( Write( "Bare.shader", kSource ), AssetHeaderReadContext{ kKnown } );
    ASSERT_FALSE( !stated ) << stated.GetError();
    EXPECT_FALSE( stated.GetValue().has_value() );

    const auto refused = ReadShaderHeader( kSource );
    ASSERT_TRUE( !refused );
    EXPECT_NE( refused.GetError().find( std::string( kShaderHeaderPrefix ) ), std::string::npos )
         << refused.GetError();
}

TEST( ShaderCommentHeader, AClaimedButMalformedHeaderIsAnErrorNamingTheFormatNotAnUnheadedFile )
{
    const auto file =
         Write( "Broken.shader", std::string( kShaderHeaderPrefix ) + R"({"Kind":)" + "\n" + kSource );
    const auto stated = ReadAssetHeaderIfStated( file, AssetHeaderReadContext{ kKnown } );
    ASSERT_TRUE( !stated );
    EXPECT_NE( stated.GetError().find( "shader comment header" ), std::string::npos ) << stated.GetError();
}
