// FO-1: the `.defoliage` foliage type (UE's UFoliageType_InstancedStaticMesh as an asset). What must hold:
// a written type reads back as written with a header GUID that survives a re-save; every way a file can be
// wrong is REFUSED by name rather than read as defaults; and the kind has its one registry row, so the
// content scan, the header check and the cooked registry all agree on what a `.defoliage` is.

#include <Engine/Assets/Serialization/FoliageType.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace Desert::Assets::Serialization;

namespace
{
    FoliageTypeData OffDefaults()
    {
        FoliageTypeData data;
        data.Mesh             = { "0123456789abcdef0123456789abcdef", "Meshes/Grass.stmesh" };
        data.Density          = 23.0f;
        data.ScaleX           = { 0.11f, 4.75f };
        data.ZOffset          = { -12.5f, 37.25f };
        data.AlignToNormal    = false; // both bools default TRUE, so false is the value a lost key would eat
        data.RandomYaw        = false;
        data.RandomPitchAngle = 18.5f;
        data.GroundSlopeAngle = { 7.0f, 62.5f };
        return data;
    }

    // Replaces the first occurrence of @p from in @p text; the test fails when it is absent, so a mutation
    // that silently did nothing cannot pass for a refusal.
    std::string Mutated( std::string text, const std::string& from, const std::string& to )
    {
        const auto at = text.find( from );
        EXPECT_NE( at, std::string::npos ) << "mutation anchor '" << from << "' not in:\n" << text;
        if ( at != std::string::npos )
            text.replace( at, from.size(), to );
        return text;
    }
} // namespace

TEST( FoliageTypeAsset, EveryFieldComesBackAndTheHeaderIsStamped )
{
    const std::string text   = WriteFoliageType( OffDefaults() );
    const auto        parsed = ParseFoliageType( text );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    FoliageTypeData read = parsed.GetValue();

    ASSERT_TRUE( read.Header.has_value() );
    EXPECT_EQ( read.Header->Kind, "FoliageType" );
    EXPECT_FALSE( read.Header->Guid.empty() );
    EXPECT_EQ( read.Header->Dependencies, std::vector<std::string>{ OffDefaults().Mesh.Guid } );

    read.Header.reset();
    EXPECT_EQ( read, OffDefaults() );
}

TEST( FoliageTypeAsset, TheGuidSurvivesAReSave )
{
    const auto first = ParseFoliageType( WriteFoliageType( OffDefaults() ) );
    ASSERT_TRUE( first ) << first.GetError();
    const auto second = ParseFoliageType( WriteFoliageType( first.GetValue() ) );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_EQ( first.GetValue().Header->Guid, second.GetValue().Header->Guid );
}

TEST( FoliageTypeAsset, ATypeWithoutAMeshStatesNoDependency )
{
    FoliageTypeData data = OffDefaults();
    data.Mesh            = {};
    const auto parsed    = ParseFoliageType( WriteFoliageType( data ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_TRUE( parsed.GetValue().Header->Dependencies.empty() );
}

TEST( FoliageTypeAsset, AFileWithoutAHeaderIsRefusedByName )
{
    const auto parsed = ParseFoliageType( R"({"Density": 6.0})" );
    ASSERT_FALSE( parsed );
    EXPECT_NE( parsed.GetError().find( "states no header" ), std::string::npos ) << parsed.GetError();
}

TEST( FoliageTypeAsset, AnotherVersionIsRefused )
{
    const std::string text   = Mutated( WriteFoliageType( OffDefaults() ), "\"FOLT\":2", "\"FOLT\":3" );
    const auto        parsed = ParseFoliageType( text );
    ASSERT_FALSE( parsed );
    EXPECT_NE( parsed.GetError().find( "FOLT 3" ), std::string::npos ) << parsed.GetError();
}

TEST( FoliageTypeAsset, AnotherKindIsRefused )
{
    const std::string text =
         Mutated( WriteFoliageType( OffDefaults() ), "\"Kind\":\"FoliageType\"", "\"Kind\":\"Retarget\"" );
    EXPECT_FALSE( ParseFoliageType( text ) );
}

TEST( FoliageTypeAsset, DependenciesThatDisagreeWithTheMeshAreRefused )
{
    const std::string text   = Mutated( WriteFoliageType( OffDefaults() ), "\"Dependencies\":[",
                                        "\"Dependencies\":[\"ffffffffffffffffffffffffffffffff\"," );
    const auto        parsed = ParseFoliageType( text );
    ASSERT_FALSE( parsed );
    EXPECT_NE( parsed.GetError().find( "Dependencies" ), std::string::npos ) << parsed.GetError();
}

TEST( FoliageTypeAsset, NumbersTheBrushCannotHonourAreRefused )
{
    FoliageTypeData zeroDensity = OffDefaults();
    zeroDensity.Density         = 0.0f;
    EXPECT_FALSE( ValidateFoliageTypeData( zeroDensity ) );

    FoliageTypeData inverted = OffDefaults();
    inverted.ScaleX          = { 2.0f, 1.0f };
    EXPECT_FALSE( ValidateFoliageTypeData( inverted ) );

    FoliageTypeData steep  = OffDefaults();
    steep.GroundSlopeAngle = { 0.0f, 120.0f };
    EXPECT_FALSE( ValidateFoliageTypeData( steep ) );

    FoliageTypeData halfMesh = OffDefaults();
    halfMesh.Mesh.Path       = "";
    EXPECT_FALSE( ValidateFoliageTypeData( halfMesh ) );

    // And the writer will not put any of them on disk.
    const auto file = std::filesystem::temp_directory_path() / "fo1_refused.defoliage";
    std::filesystem::remove( file );
    EXPECT_FALSE( SaveFoliageTypeFile( file, zeroDensity ) );
    EXPECT_FALSE( std::filesystem::exists( file ) );
}

TEST( FoliageTypeAsset, ASavedFileReadsBack )
{
    const auto dir  = std::filesystem::temp_directory_path() / "fo1_foliage_suite";
    const auto file = dir / "Grass.defoliage";
    std::filesystem::remove_all( dir );
    ASSERT_TRUE( SaveFoliageTypeFile( file, OffDefaults() ) );
    const auto text = Common::Utils::FileSystem::ReadFileContent( file );
    ASSERT_TRUE( text );
    const auto parsed = ParseFoliageType( text.GetValue() );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_FLOAT_EQ( parsed.GetValue().Density, 23.0f );
    std::filesystem::remove_all( dir );
}

// THE REGISTRY ROW: one kind, one extension, one root. A kind missing here is content the editor loads and
// the cooked game never enumerates.
TEST( FoliageTypeAsset, TheKindHasItsOneRegistryRow )
{
    using Common::Content::ContentKind;
    const auto& spec = Common::Content::KindSpec( ContentKind::FoliageType );
    EXPECT_EQ( Common::Content::KindName( ContentKind::FoliageType ), "FoliageType" );
    EXPECT_EQ( spec.Extension, std::string_view( kFoliageTypeExtension ) );
    ASSERT_NE( spec.Root, nullptr );
    EXPECT_EQ( spec.Root->filename().empty() ? spec.Root->parent_path().filename() : spec.Root->filename(),
               std::filesystem::path( "Foliage" ) );

    int rows = 0;
    for ( const auto& row : Common::Content::ContentKinds() )
        rows += row.Extension == std::string_view( kFoliageTypeExtension ) ? 1 : 0;
    EXPECT_EQ( rows, 1 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
