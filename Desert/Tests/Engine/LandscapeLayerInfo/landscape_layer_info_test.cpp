// LS-12b. The `.delayerinfo` file (UE ULandscapeLayerInfoObject): what it keeps, what it refuses, and that
// its GUID — the layer's identity and handle — survives being written again.

#include <gtest/gtest.h>

#include <TestSupport/scratch_dir.hpp>

#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>

namespace
{
    using Desert::Assets::Serialization::LandscapeLayerInfoData;
    using Desert::Assets::Serialization::ParseLandscapeLayerInfo;
    using Desert::Assets::Serialization::SaveLandscapeLayerInfoFile;
    using Desert::Assets::Serialization::ValidateLandscapeLayerInfoData;
    using Desert::Assets::Serialization::WriteLandscapeLayerInfo;

    LandscapeLayerInfoData Grass()
    {
        LandscapeLayerInfoData data;
        data.LayerName            = "Grass";
        data.Hardness             = 0.25f;
        data.NoWeightBlend        = true;
        data.LayerUsageDebugColor = glm::vec3( 0.2f, 0.7f, 0.1f );
        return data;
    }

    /// A stamped file, parsed back, so a test can change one field and write it WITHOUT re-stamping.
    LandscapeLayerInfoData StampedGrass()
    {
        auto parsed = ParseLandscapeLayerInfo( WriteLandscapeLayerInfo( Grass() ) );
        EXPECT_TRUE( parsed ) << ( parsed ? "" : parsed.GetError() );
        return parsed ? parsed.GetValue() : LandscapeLayerInfoData{};
    }

    std::string Refusal( const LandscapeLayerInfoData& data )
    {
        const auto parsed = ParseLandscapeLayerInfo( Common::Json::Write( data ) );
        return parsed ? std::string( "<accepted>" ) : parsed.GetError();
    }
} // namespace

TEST( LandscapeLayerInfo, RoundTripKeepsEveryField )
{
    const std::string text   = WriteLandscapeLayerInfo( Grass() );
    auto              parsed = ParseLandscapeLayerInfo( text );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    LandscapeLayerInfoData read = parsed.GetValue();
    ASSERT_TRUE( read.Header.has_value() );
    EXPECT_EQ( read.Header->Kind, "LandscapeLayerInfo" );
    EXPECT_TRUE( read.Header->Dependencies.empty() );
    EXPECT_FALSE( read.Header->Guid.empty() );
    read.Header.reset();
    EXPECT_EQ( read, Grass() );
}

TEST( LandscapeLayerInfo, GuidIsKeptWhenTheFileIsWrittenAgain )
{
    const LandscapeLayerInfoData first = StampedGrass();
    ASSERT_TRUE( first.Header.has_value() );
    LandscapeLayerInfoData edited = first;
    edited.Hardness               = 0.9f;
    auto again                    = ParseLandscapeLayerInfo( WriteLandscapeLayerInfo( edited ) );
    ASSERT_TRUE( again ) << again.GetError();
    EXPECT_EQ( again.GetValue().Header->Guid, first.Header->Guid );
    EXPECT_FLOAT_EQ( again.GetValue().Hardness, 0.9f );

    // A layer never loaded gets a GUID of its own: two new layers are two identities.
    EXPECT_NE( StampedGrass().Header->Guid, first.Header->Guid );
}

TEST( LandscapeLayerInfo, SavedFileParsesBack )
{
    const auto dir  = std::filesystem::temp_directory_path() / "desert_ls12b_layerinfo";
    const auto file = dir / "Landscape" / "Layers" / "Grass.delayerinfo";
    std::filesystem::remove_all( dir );
    ASSERT_TRUE( SaveLandscapeLayerInfoFile( file, Grass() ) );
    std::stringstream text;
    {
        // Closed before remove_all below: Windows refuses to delete a file that still has an open handle.
        const std::ifstream in( file, std::ios::binary );
        text << in.rdbuf();
    }
    auto parsed = ParseLandscapeLayerInfo( text.str() );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue().LayerName, "Grass" );
    std::filesystem::remove_all( dir );
}

TEST( LandscapeLayerInfo, RefusesAFileWithoutAHeader )
{
    LandscapeLayerInfoData data = Grass();
    EXPECT_NE( Refusal( data ), "<accepted>" );
    EXPECT_FALSE( ParseLandscapeLayerInfo( "" ) );
}

TEST( LandscapeLayerInfo, RefusesAnotherKind )
{
    LandscapeLayerInfoData data = StampedGrass();
    data.Header->Kind           = "UITheme";
    EXPECT_NE( Refusal( data ), "<accepted>" );
}

// LLYI 2 is refused by its number (LS-16: the GrassType field left and the corpus was rewritten as v3 in
// the same change; there is no reader of the old layout), and so is a version from the future.
TEST( LandscapeLayerInfo, RefusesAnotherVersion )
{
    for ( const uint32_t stated : { 2u, 4u } )
    {
        LandscapeLayerInfoData data = StampedGrass();
        ASSERT_FALSE( data.Header->Versions.empty() );
        for ( auto& [tag, version] : data.Header->Versions )
            version = stated;
        const std::string why = Refusal( data );
        EXPECT_NE( why, "<accepted>" );
        EXPECT_NE( why.find( std::to_string( stated ) ), std::string::npos ) << why;
    }
}

// The committed layer infos are all current (LLYI 3) and the ground layer every landscape scene lists first
// is among them.
TEST( LandscapeLayerInfo, TheCorpusLayerInfosParse )
{
    const std::filesystem::path dir =
         Desert::TestSupport::RepositoryRoot() / "Editor" / "Resources" / "Assets" / "Landscape" / "Layers";
    ASSERT_TRUE( std::filesystem::is_directory( dir ) ) << std::filesystem::absolute( dir );
    std::set<std::string> names;
    for ( const auto& entry : std::filesystem::directory_iterator( dir ) )
    {
        if ( entry.path().extension() != ".delayerinfo" )
            continue;
        std::ifstream     in( entry.path(), std::ios::binary );
        std::stringstream text;
        text << in.rdbuf();
        const auto parsed = ParseLandscapeLayerInfo( text.str() );
        ASSERT_TRUE( parsed ) << entry.path() << ": " << parsed.GetError();
        names.insert( parsed.GetValue().LayerName );
    }
    EXPECT_EQ( names, ( std::set<std::string>{ "Grass", "Ground" } ) );
}

TEST( LandscapeLayerInfo, RefusesStatedDependencies )
{
    LandscapeLayerInfoData data = StampedGrass();
    data.Header->Dependencies.push_back( data.Header->Guid );
    EXPECT_NE( Refusal( data ).find( "Dependencies" ), std::string::npos );
}

TEST( LandscapeLayerInfo, RefusesNamesAWeightPlaneCannotBeKeyedBy )
{
    LandscapeLayerInfoData empty = StampedGrass();
    empty.LayerName.clear();
    EXPECT_NE( Refusal( empty ).find( "LayerName" ), std::string::npos );

    LandscapeLayerInfoData longest = StampedGrass();
    longest.LayerName              = std::string( Desert::World::Landscape::kLandscapeMaxWeightLayerName, 'a' );
    EXPECT_EQ( Refusal( longest ), "<accepted>" );
    longest.LayerName.push_back( 'a' );
    EXPECT_NE( Refusal( longest ).find( "65 bytes" ), std::string::npos ) << Refusal( longest );
}

TEST( LandscapeLayerInfo, RefusesHardnessOutsideZeroToOne )
{
    for ( const float bad : { 1.5f, -0.1f } )
    {
        LandscapeLayerInfoData data = StampedGrass();
        data.Hardness               = bad;
        EXPECT_NE( Refusal( data ).find( "Hardness" ), std::string::npos ) << bad;
    }
    // JSON has no NaN, so a non-finite number can only reach the rule from memory.
    LandscapeLayerInfoData nan = Grass();
    nan.Hardness               = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE( ValidateLandscapeLayerInfoData( nan ) );
    LandscapeLayerInfoData edge = StampedGrass();
    edge.Hardness               = 1.0f;
    EXPECT_EQ( Refusal( edge ), "<accepted>" );
    // The writer refuses what the reader would: a bad file is never produced.
    LandscapeLayerInfoData bad = Grass();
    bad.Hardness               = 1.5f;
    EXPECT_FALSE( SaveLandscapeLayerInfoFile(
         std::filesystem::temp_directory_path() / "desert_ls12b_bad.delayerinfo", bad ) );
}

TEST( LandscapeLayerInfo, RefusesANonFiniteDebugColour )
{
    LandscapeLayerInfoData data = Grass();
    data.LayerUsageDebugColor.g = std::numeric_limits<float>::infinity();
    const auto why              = ValidateLandscapeLayerInfoData( data );
    ASSERT_FALSE( why );
    EXPECT_NE( why.GetError().find( "LayerUsageDebugColor" ), std::string::npos ) << why.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
