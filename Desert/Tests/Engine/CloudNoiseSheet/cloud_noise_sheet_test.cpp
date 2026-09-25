#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/CloudNoiseVolumeSheet.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace Desert::Assets;

namespace
{
    // A volume whose every byte ENCODES ITS OWN ADDRESS. Filling it with noise would prove the round trip
    // for noise; filling it with a position-dependent pattern proves it for the LAYOUT, because a slice
    // written to the wrong tile, a row flipped, or an axis transposed all move a byte to an address that
    // does not match its value. A test filled with a constant would pass through any of those.
    CloudNoiseVolumeData PatternedVolume( uint32_t resolution )
    {
        CloudNoiseVolumeData volume;
        volume.Params           = EmptyImportedRecipe( resolution );
        volume.Origin           = CloudNoiseVolumeOrigin::Imported;
        volume.GeneratorVersion = 0u;
        volume.Voxels.resize( static_cast<size_t>( resolution ) * resolution * resolution * 4u );

        for ( uint32_t z = 0; z < resolution; ++z )
            for ( uint32_t y = 0; y < resolution; ++y )
                for ( uint32_t x = 0; x < resolution; ++x )
                {
                    const size_t at = ( ( static_cast<size_t>( z ) * resolution + y ) * resolution + x ) * 4u;
                    volume.Voxels[at + 0] = static_cast<unsigned char>( x );
                    volume.Voxels[at + 1] = static_cast<unsigned char>( y );
                    volume.Voxels[at + 2] = static_cast<unsigned char>( z );
                    // A fourth plane that is not any single axis, so a swap of two axes cannot leave the
                    // volume looking self-consistent.
                    volume.Voxels[at + 3] = static_cast<unsigned char>( ( x * 7u + y * 13u + z * 31u ) & 0xFFu );
                }

        return volume;
    }

    CloudNoiseVolumeParams GeneratedRecipe( uint32_t resolution )
    {
        CloudNoiseVolumeParams params;
        params.Resolution                = resolution;
        params.Seed                      = 4242u;
        params.CurlStrength              = 0.25f;
        params.WispyPeriodLowFrequency   = 2.0f;
        params.WispyPeriodHighFrequency  = 4.0f;
        params.BillowPeriodLowFrequency  = 3.0f;
        params.BillowPeriodHighFrequency = 6.0f;
        return params;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The layout: two statements of one fact, and they are asserted against each other rather than typed
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseSheetLayout, TheGridHoldsExactlyOneTilePerSlice )
{
    // The relation the whole file rests on. A grid one tile short silently drops the last slice; one tile
    // long imports uninitialised pixels as cloud. Neither would fail a test of either side alone.
    for ( const uint32_t resolution : { 64u, 128u } )
    {
        const auto layout = CloudNoiseSheetLayoutFor( resolution );
        ASSERT_TRUE( layout ) << layout.GetError();

        EXPECT_EQ( layout.GetValue().TilesX * layout.GetValue().TilesY, resolution )
             << "resolution " << resolution;
        EXPECT_EQ( layout.GetValue().Width, layout.GetValue().TilesX * resolution );
        EXPECT_EQ( layout.GetValue().Height, layout.GetValue().TilesY * resolution );
    }
}

TEST( CloudNoiseSheetLayout, TheSheetHoldsExactlyTheVolumesBytes )
{
    // Sheet area times four must equal the volume's payload exactly. This is the same "two statements of
    // one fact" check the container makes between resolution and payload length, asked across the seam
    // between the container and the sheet — which is where an import could otherwise read past the end.
    for ( const uint32_t resolution : { 64u, 128u } )
    {
        const auto layout = CloudNoiseSheetLayoutFor( resolution );
        ASSERT_TRUE( layout ) << layout.GetError();

        const uint64_t sheetBytes =
             static_cast<uint64_t>( layout.GetValue().Width ) * layout.GetValue().Height * 4u;
        const uint64_t volumeBytes = static_cast<uint64_t>( resolution ) * resolution * resolution * 4u;
        EXPECT_EQ( sheetBytes, volumeBytes ) << "resolution " << resolution;
    }
}

TEST( CloudNoiseSheetLayout, AnIllegalResolutionIsRefusedByTheContainersOwnRule )
{
    // The sheet must not be a second opinion about what size a volume may be.
    for ( const uint32_t resolution : { 0u, 32u, 100u, 256u } )
    {
        const auto layout = CloudNoiseSheetLayoutFor( resolution );
        EXPECT_FALSE( layout ) << "resolution " << resolution << " should not lay out";
        EXPECT_NE( layout.GetError().find( std::to_string( resolution ) ), std::string::npos )
             << layout.GetError();
    }
}

// ---------------------------------------------------------------------------------------------------
// The round trip — the claim the whole task is judged on
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseSheet, VolumeToSheetToVolumeIsByteIdentical )
{
    for ( const uint32_t resolution : { 64u, 128u } )
    {
        const CloudNoiseVolumeData original = PatternedVolume( resolution );

        const auto sheet = EncodeCloudNoiseVolumeToSheet( original );
        ASSERT_TRUE( sheet ) << sheet.GetError();

        const auto back = DecodeCloudNoiseVolumeFromSheet( sheet.GetValue().Pixels, sheet.GetValue().Layout.Width,
                                                           sheet.GetValue().Layout.Height );
        ASSERT_TRUE( back ) << back.GetError();

        ASSERT_EQ( back.GetValue().Params.Resolution, resolution );
        ASSERT_EQ( back.GetValue().Voxels.size(), original.Voxels.size() );
        EXPECT_EQ( back.GetValue().Voxels, original.Voxels ) << "resolution " << resolution;
    }
}

TEST( CloudNoiseSheet, TheFullTripThroughTheContainerPreservesEveryPayloadByte )
{
    // The trip the owner actually asked for: an asset on disk, exported to a picture, edited elsewhere,
    // brought back, and written as an asset again. Everything but the disk is exercised here.
    const CloudNoiseVolumeData original = PatternedVolume( 64u );

    const std::vector<unsigned char> encodedBefore = EncodeCloudNoisePayload( original );
    const auto                       decodedBefore = DecodeCloudNoisePayload( encodedBefore );
    ASSERT_TRUE( decodedBefore ) << decodedBefore.GetError();

    const auto sheet = EncodeCloudNoiseVolumeToSheet( decodedBefore.GetValue() );
    ASSERT_TRUE( sheet ) << sheet.GetError();

    const auto reimported = DecodeCloudNoiseVolumeFromSheet(
         sheet.GetValue().Pixels, sheet.GetValue().Layout.Width, sheet.GetValue().Layout.Height );
    ASSERT_TRUE( reimported ) << reimported.GetError();

    const std::vector<unsigned char> encodedAfter = EncodeCloudNoisePayload( reimported.GetValue() );
    const auto                       decodedAfter = DecodeCloudNoisePayload( encodedAfter );
    ASSERT_TRUE( decodedAfter ) << decodedAfter.GetError();

    EXPECT_EQ( decodedAfter.GetValue().Voxels, original.Voxels );

    // AND THE WHOLE PAYLOAD IS IDENTICAL TOO — but only because the volume that made the trip was ALREADY
    // imported. That is the honest statement of what round-trips: the voxels always, the payload only when
    // there was no recipe to lose. The next test is the other half.
    EXPECT_EQ( encodedAfter, encodedBefore );
}

TEST( CloudNoiseSheet, AGeneratedVolumeLosesItsRecipeAndSaysSo )
{
    // WHAT THE SHEET CANNOT CARRY, asserted rather than left as a sentence in a header. A picture has
    // nowhere to put a seed, so a generated volume that goes out and comes back is the same voxels with no
    // recipe — and the file must say "imported" rather than keep claiming a recipe that no longer produced
    // it. A build that silently preserved the recipe here would be lying about reproducibility.
    CloudNoiseVolumeData generated = PatternedVolume( 64u );
    generated.Params               = GeneratedRecipe( 64u );
    generated.Origin               = CloudNoiseVolumeOrigin::Generated;
    generated.GeneratorVersion     = kCloudNoiseGeneratorVersion;

    const auto sheet = EncodeCloudNoiseVolumeToSheet( generated );
    ASSERT_TRUE( sheet ) << sheet.GetError();

    const auto back = DecodeCloudNoiseVolumeFromSheet( sheet.GetValue().Pixels, sheet.GetValue().Layout.Width,
                                                       sheet.GetValue().Layout.Height );
    ASSERT_TRUE( back ) << back.GetError();

    EXPECT_EQ( back.GetValue().Voxels, generated.Voxels ) << "the voxels must survive regardless";

    EXPECT_EQ( back.GetValue().Origin, CloudNoiseVolumeOrigin::Imported );
    EXPECT_EQ( back.GetValue().Params.Seed, 0u );
    EXPECT_EQ( back.GetValue().Params.WispyPeriodLowFrequency, 0.0f );
    EXPECT_EQ( back.GetValue().Params.CurlStrength, 0.0f );
    EXPECT_EQ( back.GetValue().GeneratorVersion, 0u ) << "no version of our maths produced these voxels";

    // The resolution is the one thing the pixels genuinely do state, so it must come back real.
    EXPECT_EQ( back.GetValue().Params.Resolution, 64u );
}

TEST( CloudNoiseSheet, SliceZeroIsTheTopLeftTileAndSlicesRunLeftToRight )
{
    // The convention, pinned. It is arbitrary but it must not DRIFT: a build that started writing slices
    // top-to-bottom would still round-trip perfectly through itself while silently disagreeing with every
    // sheet ever exported by an older build and with the tool the artist uses.
    const CloudNoiseVolumeData volume = PatternedVolume( 64u );

    const auto sheet = EncodeCloudNoiseVolumeToSheet( volume );
    ASSERT_TRUE( sheet ) << sheet.GetError();

    const auto&    image = sheet.GetValue();
    const uint32_t n     = image.Layout.Resolution;

    // Slice z lives at tile (z % TilesX, z / TilesX). The blue plane holds z, so the top-left texel of each
    // tile is the direct statement of that mapping.
    for ( uint32_t z = 0; z < n; ++z )
    {
        const uint32_t originX = ( z % image.Layout.TilesX ) * n;
        const uint32_t originY = ( z / image.Layout.TilesX ) * n;
        const size_t   at      = ( static_cast<size_t>( originY ) * image.Layout.Width + originX ) * 4u;

        EXPECT_EQ( image.Pixels[at + 2], static_cast<unsigned char>( z ) )
             << "slice " << z << " is not in its tile";
        EXPECT_EQ( image.Pixels[at + 0], 0u ) << "x should be 0 at a tile's left edge";
        EXPECT_EQ( image.Pixels[at + 1], 0u ) << "y should be 0 at a tile's top edge";
    }
}

// ---------------------------------------------------------------------------------------------------
// The refusals — the requirement was that a mismatch is NAMED, never quietly resampled
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseSheet, ASheetOfTheWrongSizeIsRefusedWithBothNumbers )
{
    // The teamlead's explicit requirement: a sheet that does not make a cube is a named refusal carrying
    // the numbers, not a silent interpolation.
    const std::vector<std::pair<uint32_t, uint32_t>> wrong = {
         { 2000u, 1000u }, // nearly the 128^3 sheet, which is exactly the tempting case to "just fit"
         { 2048u, 1023u }, // one row short
         { 1024u, 2048u }, // the right area, transposed
         { 511u, 511u },   // nearly the 64^3 sheet
    };

    for ( const auto& [width, height] : wrong )
    {
        const std::vector<unsigned char> pixels( static_cast<size_t>( width ) * height * 4u, 0u );
        const auto                       refused = DecodeCloudNoiseVolumeFromSheet( pixels, width, height );

        EXPECT_FALSE( refused ) << width << "x" << height << " should not import";
        EXPECT_NE( refused.GetError().find( std::to_string( width ) ), std::string::npos ) << refused.GetError();
        EXPECT_NE( refused.GetError().find( std::to_string( height ) ), std::string::npos ) << refused.GetError();

        // ...and it must say what WOULD have worked, or the artist is sent away to guess.
        EXPECT_NE( refused.GetError().find( "2048x1024" ), std::string::npos ) << refused.GetError();
    }
}

TEST( CloudNoiseSheet, APixelBufferThatDisagreesWithItsOwnSizeIsRefused )
{
    // Two statements of one fact again: the declared width and height, and the number of bytes actually
    // handed over. Trusting either alone is how an import reads past the end of a buffer.
    const std::vector<unsigned char> tooFew( 512u * 512u * 4u - 4u, 0u );
    const auto                       refused = DecodeCloudNoiseVolumeFromSheet( tooFew, 512u, 512u );

    EXPECT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( std::to_string( 512u * 512u * 4u ) ), std::string::npos )
         << refused.GetError();
}

// ---------------------------------------------------------------------------------------------------
// The container's new field, and the migration that keeps old files readable exactly once
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseContainer, TheOriginSurvivesTheContainer )
{
    for ( const auto origin : { CloudNoiseVolumeOrigin::Generated, CloudNoiseVolumeOrigin::Imported } )
    {
        CloudNoiseVolumeData volume = PatternedVolume( 64u );
        volume.Origin               = origin;
        volume.Params =
             origin == CloudNoiseVolumeOrigin::Generated ? GeneratedRecipe( 64u ) : EmptyImportedRecipe( 64u );
        volume.GeneratorVersion = origin == CloudNoiseVolumeOrigin::Generated ? kCloudNoiseGeneratorVersion : 0u;

        const auto encoded = EncodeCloudNoiseVolume( volume );
        ASSERT_TRUE( encoded ) << encoded.GetError();
        const auto decoded = DecodeCloudNoiseVolume( encoded.GetValue() );
        ASSERT_TRUE( decoded ) << decoded.GetError();
        EXPECT_EQ( decoded.GetValue().Origin, origin );
    }
}

// The finding is inside gtest's TEST macro (it registers the test through a raw `new` handed to a
// non-owner parameter), not in this test; every TEST line carries it, this one is on a changed line.
// NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
TEST( CloudNoiseContainer, ThePayloadIsTheSizeTheConstantClaims )
{
    const CloudNoiseVolumeData       volume  = PatternedVolume( 64u );
    const std::vector<unsigned char> payload = EncodeCloudNoisePayload( volume );

    EXPECT_EQ( payload.size(), kCloudNoiseHeaderSize + volume.Voxels.size() );
}

TEST( CloudNoiseContainer, AnImportedFileCarryingARecipeIsRefused )
{
    // The two halves of the header disagreeing about where the voxels came from. Hand-built, because the
    // encoder cannot produce it — which is the point: this guards against a file from somewhere else.
    CloudNoiseVolumeData volume = PatternedVolume( 64u );
    volume.Params               = GeneratedRecipe( 64u );
    volume.Origin               = CloudNoiseVolumeOrigin::Generated;

    std::vector<unsigned char> bytes = EncodeCloudNoisePayload( volume );
    bytes[52]                        = static_cast<unsigned char>( CloudNoiseVolumeOrigin::Imported );

    const auto refused = DecodeCloudNoisePayload( bytes );
    EXPECT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "4242" ), std::string::npos ) << refused.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
