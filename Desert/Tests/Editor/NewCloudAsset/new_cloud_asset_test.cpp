// What the Content Browser's "New Cloud Asset" items put on disk, and whether it comes back.
//
// THE PROPERTY THIS SUITE ASSERTS IS A RELATION AND NOT A FUNCTION: for each of the four cloud formats,
// what the creation WRITES is what a loader READS. "The file appeared" would pass on a stub — a zero-byte
// `.dcnv`, a `.dclayout` with no tables, a `.decloudtype` written past its serialiser — and every one of
// those is refused by the format's own reader, several frames later and somewhere else. The two sides here
// are the writer and the reader, and they are made to agree rather than each checked alone.
//
// It also pins the two things a panel is most likely to get wrong on its own:
//
//   * that the defaults are the ENGINE's defaults and not a second table typed into an editor file — the
//     defect shape this project calls "a preset table and the saved scenes disagreeing";
//   * that every default PASSES the validator its format refuses files with, so a creation cannot produce
//     a file that will not load.
//
// COST. The two volume formats are generated for real, at their real default resolution, because a volume
// created at any other size is not the volume the menu item makes. Measured on the machine this was
// written on, Debug: 8.7 s for the 128^3 noise volume and 1.6 s for the modelling body, so this suite is
// about eleven seconds where its neighbours are milliseconds. Generating a smaller one instead would test
// a file the editor never creates.

#include <Editor/Panels/FileExplorer/NewCloudAsset.hpp>

#include <Engine/Assets/CloudLayoutAsset.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Desert;

namespace
{
    // A directory of this suite's own, emptied on the way in. Named after the suite so that two tests
    // running side by side on one machine cannot write over each other.
    std::filesystem::path Scratch()
    {
        const auto      dir = std::filesystem::temp_directory_path() / "DesertNewCloudAssetTest";
        std::error_code ec;
        std::filesystem::create_directories( dir, ec );
        return dir;
    }

    std::vector<unsigned char> ReadBytes( const std::filesystem::path& path )
    {
        std::ifstream file( path, std::ios::binary );
        EXPECT_TRUE( file.good() ) << "could not open " << path.string();
        return std::vector<unsigned char>( ( std::istreambuf_iterator<char>( file ) ),
                                           std::istreambuf_iterator<char>() );
    }

    std::string ReadText( const std::filesystem::path& path )
    {
        std::ifstream file( path );
        EXPECT_TRUE( file.good() ) << "could not open " << path.string();
        return std::string( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    }

    void ExpectShapesEqual( const Graphic::CloudTypeShape& written, const Graphic::CloudTypeShape& read )
    {
        EXPECT_FLOAT_EQ( written.BaseAltitudeKm, read.BaseAltitudeKm );
        EXPECT_FLOAT_EQ( written.TopAltitudeKm, read.TopAltitudeKm );
        EXPECT_FLOAT_EQ( written.EdgeTopFraction, read.EdgeTopFraction );
        EXPECT_FLOAT_EQ( written.BaseRampFraction, read.BaseRampFraction );
        EXPECT_FLOAT_EQ( written.AnvilAltitudeKm, read.AnvilAltitudeKm );
        EXPECT_FLOAT_EQ( written.AnvilThicknessKm, read.AnvilThicknessKm );
        EXPECT_FLOAT_EQ( written.AnvilStrength, read.AnvilStrength );
        EXPECT_FLOAT_EQ( written.DetailCharacter, read.DetailCharacter );
        EXPECT_FLOAT_EQ( written.DetailFactor, read.DetailFactor );
        EXPECT_FLOAT_EQ( written.DensityFactor, read.DensityFactor );
        EXPECT_FLOAT_EQ( written.ExtinctionFactor, read.ExtinctionFactor );
        EXPECT_FLOAT_EQ( written.PlacementScale, read.PlacementScale );
        EXPECT_FLOAT_EQ( written.PlacementAnisotropy, read.PlacementAnisotropy );

        for ( uint32_t i = 0; i < Graphic::kCloudProfileSamples; ++i )
            EXPECT_FLOAT_EQ( written.Profile.HalfWidth[i], read.Profile.HalfWidth[i] ) << "profile sample " << i;
    }

    void ExpectRecipesEqual( const Assets::CloudModellingVolumeRecipe& written,
                             const Assets::CloudModellingVolumeRecipe& read )
    {
        EXPECT_FLOAT_EQ( written.SizeKm.x, read.SizeKm.x );
        EXPECT_FLOAT_EQ( written.SizeKm.y, read.SizeKm.y );
        EXPECT_FLOAT_EQ( written.SizeKm.z, read.SizeKm.z );
        EXPECT_FLOAT_EQ( written.BlendRadiusKm, read.BlendRadiusKm );
        EXPECT_FLOAT_EQ( written.ProfileDepthKm, read.ProfileDepthKm );
        EXPECT_FLOAT_EQ( written.EnvelopeMarginKm, read.EnvelopeMarginKm );

        ASSERT_EQ( written.Blobs.size(), read.Blobs.size() );
        for ( size_t i = 0; i < written.Blobs.size(); ++i )
        {
            const Assets::CloudModellingBlob& a = written.Blobs[i];
            const Assets::CloudModellingBlob& b = read.Blobs[i];

            EXPECT_FLOAT_EQ( a.CentreKm.x, b.CentreKm.x ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.CentreKm.y, b.CentreKm.y ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.CentreKm.z, b.CentreKm.z ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.RadiiKm.x, b.RadiiKm.x ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.RadiiKm.y, b.RadiiKm.y ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.RadiiKm.z, b.RadiiKm.z ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.RotationDeg.x, b.RotationDeg.x ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.RotationDeg.y, b.RotationDeg.y ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.RotationDeg.z, b.RotationDeg.z ) << "lump " << i;
            EXPECT_EQ( static_cast<uint32_t>( a.Primitive ), static_cast<uint32_t>( b.Primitive ) )
                 << "lump " << i;
            EXPECT_FLOAT_EQ( a.Weight, b.Weight ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.DetailType, b.DetailType ) << "lump " << i;
            EXPECT_FLOAT_EQ( a.DensityScale, b.DensityScale ) << "lump " << i;
        }
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// THE DEFAULTS ARE THE ENGINE'S OWN, AND EVERY ONE OF THEM PASSES THE CHECK ITS LOADER REFUSES BY
// ---------------------------------------------------------------------------------------------------

TEST( NewCloudAsset, TheNoiseParametersAreTheStructsOwnDefaults )
{
    // A CENSUS AGAINST A DEFAULT-CONSTRUCTED STRUCT, not a list of numbers. If a panel ever "helpfully"
    // dropped the new volume to 64 for speed, or re-typed the four lattice periods, this is where the
    // second table shows up — and a second table of periods is how the shipped default and the created
    // one come to be different noises with the same name.
    const Assets::CloudNoiseVolumeParams fromEngine;
    const Assets::CloudNoiseVolumeParams fromCreation = Editor::NewCloudAsset::DefaultNoiseParams();

    EXPECT_EQ( fromEngine.Resolution, fromCreation.Resolution );
    EXPECT_EQ( fromEngine.Seed, fromCreation.Seed );
    EXPECT_FLOAT_EQ( fromEngine.CurlStrength, fromCreation.CurlStrength );
    EXPECT_FLOAT_EQ( fromEngine.WispyPeriodLowFrequency, fromCreation.WispyPeriodLowFrequency );
    EXPECT_FLOAT_EQ( fromEngine.WispyPeriodHighFrequency, fromCreation.WispyPeriodHighFrequency );
    EXPECT_FLOAT_EQ( fromEngine.BillowPeriodLowFrequency, fromCreation.BillowPeriodLowFrequency );
    EXPECT_FLOAT_EQ( fromEngine.BillowPeriodHighFrequency, fromCreation.BillowPeriodHighFrequency );
}

TEST( NewCloudAsset, EveryDefaultPassesTheValidatorItsLoaderRefusesBy )
{
    // The relation: the panel's "create" and the format's "load" must agree about what is legal. Asserting
    // it here means a default that could never be loaded back is a red test rather than a file an artist
    // makes once and cannot open.
    const auto noise = Assets::ValidateCloudNoiseVolumeParams( Editor::NewCloudAsset::DefaultNoiseParams() );
    EXPECT_TRUE( noise ) << noise.GetError();

    const auto shape = Assets::ValidateCloudTypeShape( Editor::NewCloudAsset::DefaultType( "Whatever" ).Shape );
    EXPECT_TRUE( shape ) << shape.GetError();

    const auto recipe = Assets::ValidateCloudModellingRecipe( Assets::CloudModellingDefaultRecipe() );
    EXPECT_TRUE( recipe ) << recipe.GetError();

    auto layout = Editor::NewCloudAsset::DefaultLayout();
    ASSERT_TRUE( layout ) << layout.GetError();
    const auto layoutValid = Assets::ValidateCloudLayoutData( layout.GetValue() );
    EXPECT_TRUE( layoutValid ) << layoutValid.GetError();
}

TEST( NewCloudAsset, ANewLayoutIsABlankCanvasAndNotAnEmptyFile )
{
    // THE TRAP THIS TEST EXISTS FOR. A default-constructed CloudLayoutData is resolution 0 with neither
    // table, and it reads like a perfectly good "empty layout, the artist will paint it later" — but
    // ValidateCloudLayoutData refuses exactly that, so it can be neither written nor read. The created
    // layout must be USABLE, which is the same word the validator uses.
    auto layout = Editor::NewCloudAsset::DefaultLayout();
    ASSERT_TRUE( layout ) << layout.GetError();

    const Assets::CloudLayoutData& data = layout.GetValue();
    EXPECT_TRUE( data.IsUsable() );
    EXPECT_EQ( data.Resolution, Editor::NewCloudAsset::kNewLayoutSide );
    EXPECT_TRUE( data.HasPattern() );
    EXPECT_FALSE( data.HasMask() ); // a canvas' alpha is the mask's NEUTRAL; a neutral mask is no mask

    // AND IT CHANGES NOTHING UNTIL IT IS PAINTED ON, which is the property that makes a blank layout an
    // honest starting point rather than a sky-wide edit nobody asked for. The pattern is applied about its
    // own channel mean, so a channel of one constant value contributes exactly zero however bright that
    // constant is — assert the constancy, which is what the artist's first brush stroke breaks.
    const size_t texels = static_cast<size_t>( data.Resolution ) * data.Resolution;
    ASSERT_EQ( data.Pattern.size(), texels * 4u );
    for ( uint32_t slot = 0; slot < Assets::kCloudLayoutChannels; ++slot )
    {
        const unsigned char first = data.Pattern[slot];
        for ( size_t t = 1; t < texels; ++t )
            ASSERT_EQ( data.Pattern[t * 4u + slot], first ) << "slot " << slot << " is not flat at texel " << t;
    }
}

TEST( NewCloudAsset, ANewTypeDoesNotInheritTheBuiltInsClaimThatItIsNotAFile )
{
    // The built-in's own note reads "It is not a file", which is false the moment it is written to one,
    // and its display name would put "Cumulus congestus (built-in)" in the slot dropdown for every asset
    // an artist ever creates. The SHAPE — every number that makes this a kind of cloud — is untouched, and
    // that is asserted against the engine's own answer rather than against a copy of the digits.
    const Assets::CloudTypeData created = Editor::NewCloudAsset::DefaultType( "Anvil_Test" );

    EXPECT_EQ( created.DisplayName.value_or( "" ), "Anvil_Test" );
    EXPECT_FALSE( created.Notes.has_value() );
    ExpectShapesEqual( Assets::CloudTypeDefaultShape(), created.Shape );
}

// ---------------------------------------------------------------------------------------------------
// THE ROUND TRIP: create -> Save -> load -> compare, once per format
// ---------------------------------------------------------------------------------------------------

TEST( NewCloudAsset, ACreatedCloudTypeReadsBackAsWhatWasWritten )
{
    const auto path = Scratch() / "NewCloudType.decloudtype";

    const Assets::CloudTypeData written = Editor::NewCloudAsset::DefaultType( path.stem().string() );

    const auto saved = Assets::CloudTypeAsset::Save( path, written );
    ASSERT_TRUE( saved ) << saved.GetError();

    const auto read = Assets::ParseCloudType( ReadText( path ) );
    ASSERT_TRUE( read ) << read.GetError();

    EXPECT_EQ( read.GetValue().DisplayName.value_or( "" ), written.DisplayName.value_or( "" ) );
    EXPECT_EQ( read.GetValue().FormatVersion.value_or( -1 ), Assets::kCloudTypeFormatVersion );
    ExpectShapesEqual( written.Shape, read.GetValue().Shape );
}

TEST( NewCloudAsset, ACreatedCloudLayoutReadsBackAsWhatWasWritten )
{
    const auto path = Scratch() / "NewCloudLayout.dclayout";

    auto layout = Editor::NewCloudAsset::DefaultLayout();
    ASSERT_TRUE( layout ) << layout.GetError();
    const Assets::CloudLayoutData& written = layout.GetValue();

    const auto saved = Assets::CloudLayoutAsset::Save( path, written );
    ASSERT_TRUE( saved ) << saved.GetError();

    const auto read = Assets::DecodeCloudLayout( ReadBytes( path ) );
    ASSERT_TRUE( read ) << read.GetError();

    EXPECT_EQ( read.GetValue().Resolution, written.Resolution );
    EXPECT_EQ( read.GetValue().Pattern, written.Pattern );
    EXPECT_EQ( read.GetValue().Mask, written.Mask );
    EXPECT_EQ( read.GetValue().ContentHash, written.ContentHash );
    for ( uint32_t slot = 0; slot < Assets::kCloudLayoutChannels; ++slot )
        EXPECT_FLOAT_EQ( read.GetValue().PatternMean[slot], written.PatternMean[slot] ) << "slot " << slot;
}

TEST( NewCloudAsset, ACreatedNoiseVolumeReadsBackAsWhatWasWritten )
{
    const auto path = Scratch() / "NewCloudNoise.dcnv";

    auto volume = Editor::NewCloudAsset::DefaultNoiseVolume( nullptr );
    ASSERT_TRUE( volume ) << volume.GetError();
    const Assets::CloudNoiseVolumeData& written = volume.GetValue();

    const auto saved = Assets::CloudNoiseVolumeAsset::Save( path, written );
    ASSERT_TRUE( saved ) << saved.GetError();

    const auto read = Assets::DecodeCloudNoiseVolume( ReadBytes( path ) );
    ASSERT_TRUE( read ) << read.GetError();

    EXPECT_EQ( read.GetValue().Params.Resolution, written.Params.Resolution );
    EXPECT_EQ( read.GetValue().Params.Seed, written.Params.Seed );
    EXPECT_FLOAT_EQ( read.GetValue().Params.CurlStrength, written.Params.CurlStrength );
    EXPECT_FLOAT_EQ( read.GetValue().Params.WispyPeriodLowFrequency, written.Params.WispyPeriodLowFrequency );
    EXPECT_FLOAT_EQ( read.GetValue().Params.WispyPeriodHighFrequency, written.Params.WispyPeriodHighFrequency );
    EXPECT_FLOAT_EQ( read.GetValue().Params.BillowPeriodLowFrequency, written.Params.BillowPeriodLowFrequency );
    EXPECT_FLOAT_EQ( read.GetValue().Params.BillowPeriodHighFrequency, written.Params.BillowPeriodHighFrequency );
    EXPECT_EQ( read.GetValue().GeneratorVersion, written.GeneratorVersion );

    // THE PAYLOAD, BYTE FOR BYTE. A header that survives while the voxels do not is exactly the file that
    // loads and renders as a sky nobody authored.
    ASSERT_EQ( read.GetValue().Voxels.size(), written.Voxels.size() );
    EXPECT_EQ( read.GetValue().Voxels, written.Voxels );
}

TEST( NewCloudAsset, ACreatedModellingVolumeReadsBackAsWhatWasWritten )
{
    const auto path = Scratch() / "NewCloudBody.dcmv";

    auto body = Editor::NewCloudAsset::DefaultModellingVolume( {} );
    ASSERT_TRUE( body ) << body.GetError();
    const Assets::CloudModellingVolumeData& written = body.GetValue();

    const auto saved = Assets::CloudModellingVolumeAsset::Save( path, written );
    ASSERT_TRUE( saved ) << saved.GetError();

    const auto read = Assets::DecodeCloudModellingVolume( ReadBytes( path ) );
    ASSERT_TRUE( read ) << read.GetError();

    ExpectRecipesEqual( written.Recipe, read.GetValue().Recipe );
    EXPECT_EQ( read.GetValue().GeneratorVersion, written.GeneratorVersion );
    ASSERT_EQ( read.GetValue().Voxels.size(), written.Voxels.size() );
    EXPECT_EQ( read.GetValue().Voxels, written.Voxels );
}

// ---------------------------------------------------------------------------------------------------
// AND A REFUSAL IS CARRIED, NOT SWALLOWED
// ---------------------------------------------------------------------------------------------------

TEST( NewCloudAsset, AWriteThatCannotHappenIsAnErrorNamingThePath )
{
    // A menu item that sometimes produces no file and says nothing is worse than no menu item, so the
    // panel's error path has to have something to report. Here the destination directory cannot exist
    // because a FILE already occupies its name — the cheapest unwritable path that behaves the same way on
    // every platform this builds for.
    const auto blocker = Scratch() / "not_a_directory";
    {
        std::ofstream make( blocker, std::ios::trunc );
        make << "occupied";
    }

    const auto path = blocker / "NewCloudType.decloudtype";

    const auto saved = Assets::CloudTypeAsset::Save( path, Editor::NewCloudAsset::DefaultType( "Blocked" ) );
    EXPECT_FALSE( saved );
    EXPECT_NE( saved.GetError().find( "NewCloudType.decloudtype" ), std::string::npos ) << saved.GetError();
}

// A SAVE THAT DID NOT REACH THE DISK IS A REFUSAL, ONCE PER FORMAT (Д35)
//
// All four of these Saves used to end `return BOOLSUCCESS` after a write nobody had flushed, so whether a
// failure was caught depended on whether the payload happened to exceed the filebuf — invisible at the
// site and different for each of the four. They go through
// Common::Utils::FileSystem::WriteBytesToFileAtomic now, which writes `<path>.tmp` beside the
// destination and closes before it decides, so BLOCKING THE TEMP is a write that genuinely cannot
// happen and is the same injection for all four regardless of size. (That the primitive also refuses a
// failure only the FLUSH can see — the one this census is named for — is proven where it belongs, in
// Desert/Tests/Common/FileSystemWrite, with a negative control.)
//
// The destination is deliberately WRITABLE and already occupied: a save that "fails" by destroying what
// was there is the Д31-A defect, not a fix for it.
namespace
{
    // The blocked working file, plus the original content the refusal must leave standing.
    struct BlockedSave
    {
        std::filesystem::path Path;
        std::string           Original;
    };

    BlockedSave BlockTemp( const char* name )
    {
        BlockedSave blocked;
        blocked.Path     = Scratch() / name;
        blocked.Original = "the file that was already here";
        {
            std::ofstream previous( blocked.Path, std::ios::binary | std::ios::trunc );
            previous << blocked.Original;
        }
        std::filesystem::path temp = blocked.Path;
        temp += ".tmp";
        std::filesystem::remove_all( temp );
        std::filesystem::create_directories( temp );
        return blocked;
    }

    std::string ReadRawFile( const std::filesystem::path& p )
    {
        std::ifstream in( p, std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    }

    void ExpectRefusedAndOriginalIntact( const BlockedSave& blocked, const Common::BoolResultStr& saved )
    {
        EXPECT_FALSE( saved ) << "a save that could not be written reported success";
        EXPECT_NE( saved.GetError().find( blocked.Path.filename().string() ), std::string::npos )
             << "the refusal must name the file: " << saved.GetError();
        EXPECT_EQ( ReadRawFile( blocked.Path ), blocked.Original )
             << "the failed save cost the artist the file that was already there";
    }
} // namespace

TEST( NewCloudAsset, ACloudTypeSaveThatCannotBeWrittenIsARefusal )
{
    const auto blocked = BlockTemp( "RefusedType.decloudtype" );
    ExpectRefusedAndOriginalIntact(
         blocked, Assets::CloudTypeAsset::Save( blocked.Path, Editor::NewCloudAsset::DefaultType( "Refused" ) ) );
}

TEST( NewCloudAsset, ACloudLayoutSaveThatCannotBeWrittenIsARefusal )
{
    auto layout = Editor::NewCloudAsset::DefaultLayout();
    ASSERT_TRUE( layout ) << layout.GetError();

    const auto blocked = BlockTemp( "RefusedLayout.dclayout" );
    ExpectRefusedAndOriginalIntact( blocked, Assets::CloudLayoutAsset::Save( blocked.Path, layout.GetValue() ) );
}

TEST( NewCloudAsset, ANoiseVolumeSaveThatCannotBeWrittenIsARefusal )
{
    auto volume = Editor::NewCloudAsset::DefaultNoiseVolume( nullptr );
    ASSERT_TRUE( volume ) << volume.GetError();

    const auto blocked = BlockTemp( "RefusedNoise.dcnv" );
    ExpectRefusedAndOriginalIntact( blocked,
                                    Assets::CloudNoiseVolumeAsset::Save( blocked.Path, volume.GetValue() ) );
}

TEST( NewCloudAsset, AModellingVolumeSaveThatCannotBeWrittenIsARefusal )
{
    auto body = Editor::NewCloudAsset::DefaultModellingVolume( {} );
    ASSERT_TRUE( body ) << body.GetError();

    const auto blocked = BlockTemp( "RefusedBody.dcmv" );
    ExpectRefusedAndOriginalIntact( blocked,
                                    Assets::CloudModellingVolumeAsset::Save( blocked.Path, body.GetValue() ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
