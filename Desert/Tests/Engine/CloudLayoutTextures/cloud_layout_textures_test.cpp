// THE TWO LAYOUT TEXTURES (O-4): the pattern says WHERE the clouds are, the mask says how much to ADD or
// REMOVE, and they are two pictures, two material inputs and two independent sources at the bake — which
// is Unreal's own arrangement (`Layout_CloudGlobalPattern` and `Layout_GlobalCloudMask`,
// Docs/Clouds/RESEARCH_LAYOUT_TEXTURES.md §1.1 and §3).
//
// WHAT THIS SUITE IS FOR, AND WHY A ROUND TRIP IS NOT IT. Task O-3 measured the trap on the noise sheet:
// permuting the tile order in BOTH codecs at once leaves the round trip perfect while the agreement about
// what is on disk changes underneath it. The same trap is available here twice over — the channel that
// carries species slot k, and the byte that means "change nothing" — so both are asserted against
// LITERALS, in the direction a file leaves this engine, and the round trip sits beside them rather than
// standing in for them.
//
// And every claim about the MASK is a claim about COVERAGE rather than about pixels: neutral changes
// nothing, brighter adds, darker removes. Those are relations between a table and a baked cell, so the
// generator is compiled in and the assertions run through it.

#include <Engine/Assets/CloudLayout.hpp>
#include <Engine/Assets/CloudProceduralVolume.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace Desert::Assets;

namespace
{
    constexpr uint32_t kSide = 32u;

    /// A canvas of @p side with a pattern that differs in every channel, so a channel swapped anywhere
    /// along the way is a wrong NUMBER rather than a picture that still looks plausible.
    ///
    /// Slot k is filled with `10 + 40*k + x`, which is unique per (slot, column) and stays inside a byte at
    /// the sizes this suite uses. A test that filled every channel with the same figure could not tell R
    /// from A at all, which is exactly the mistake the fixture has to make impossible.
    CloudLayoutCanvas DistinctCanvas( uint32_t side = kSide )
    {
        auto made = MakeCloudLayoutCanvas( side );
        EXPECT_TRUE( made ) << ( made ? "" : made.GetError() );
        CloudLayoutCanvas canvas = made ? made.ExtractValue() : CloudLayoutCanvas{};

        for ( uint32_t y = 0; y < side; ++y )
            for ( uint32_t x = 0; x < side; ++x )
                for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
                    canvas.Pattern[( static_cast<size_t>( y ) * side + x ) * kCloudLayoutChannels + slot] =
                         static_cast<unsigned char>( 10u + 40u * slot + x );

        return canvas;
    }

    /// The bake parameters this suite measures coverage with: one species, one lattice, nothing procedural
    /// left switched on that could be mistaken for the painting's work.
    CloudProceduralFieldParams Params()
    {
        Desert::Graphic::CloudTypeShape shape{};
        shape.BaseAltitudeKm   = 2.20f;
        shape.TopAltitudeKm    = 5.80f;
        shape.EdgeTopFraction  = 0.15f;
        shape.BaseRampFraction = 0.04f;
        shape.Profile          = Desert::Graphic::CloudProfileFromTaper( 0.50f );
        shape.DensityFactor    = 1.15f;

        CloudProceduralFieldParams params;
        params.RegionSizeKm      = 48.0f;
        params.LayerBottomKm     = 2.20f;
        params.LayerThicknessKm  = 3.60f;
        params.BlendRadiusKm     = 0.06f;
        params.ProfileDepthKm    = 0.36f;
        params.Coverage          = 0.50f;
        params.CoverageContrast  = 1.0f;
        params.Seed              = 7u;
        params.PatchTileKm       = 21.0f;
        params.PatchStrength     = 0.0f; // the painting is the subject; the hash must not be in the picture
        params.WindAxis          = glm::vec2( 1.0f, 0.0f );
        params.ResolvableChordKm = 0.125f;

        CloudProceduralSpecies species;
        species.Shape      = shape;
        species.CellKm     = 3.0f;
        species.Anisotropy = 1.0f;
        params.Species.push_back( species );

        return params;
    }

    /// The coverage the bake gives one cell of the map, which is the number every mask claim is about.
    /// Taken through BuildCloudLayoutPreview because that calls the bake's own CloudCellCoverage — a second
    /// evaluation written here would be a mirror with one reader, which this engine has a rule against.
    std::vector<float> CoverageMap( const CloudProceduralFieldParams& params )
    {
        auto mapped = BuildCloudLayoutPreview( params, 0u, params.RegionSizeKm, 64u );
        EXPECT_TRUE( mapped ) << ( mapped ? "" : mapped.GetError() );
        return mapped ? mapped.GetValue().Coverage : std::vector<float>{};
    }

    std::shared_ptr<const CloudLayoutData> Share( CloudLayoutData data )
    {
        return std::make_shared<const CloudLayoutData>( std::move( data ) );
    }
} // namespace

// =====================================================================================================
// WHAT LEAVES THIS ENGINE, stated against literals
// =====================================================================================================

// CHANNEL k OF THE EXPORTED PATTERN IS SPECIES SLOT k, and this is the agreement a round trip cannot
// pin. Reversing the channel order in the exporter AND the importer at once would leave every round-trip
// test below green while every picture this engine has ever written stopped meaning what it meant — the
// O-3 finding, transplanted. The literal is the guard.
TEST( CloudLayoutTextures, TheExportedPatternPutsSpeciesSlotKInChannelK )
{
    const CloudLayoutCanvas canvas = DistinctCanvas();

    auto picture = EncodeCloudLayoutCanvasPatternToImage( canvas );
    ASSERT_TRUE( picture ) << picture.GetError();
    ASSERT_EQ( picture.GetValue().Side, kSide );
    ASSERT_EQ( picture.GetValue().Pixels.size(), static_cast<size_t>( kSide ) * kSide * 4u );

    for ( uint32_t x = 0; x < kSide; ++x )
        for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
            ASSERT_EQ( picture.GetValue().Pixels[( static_cast<size_t>( 5u ) * kSide + x ) * 4u + slot],
                       static_cast<unsigned char>( 10u + 40u * slot + x ) )
                 << "channel " << slot << " of the exported pattern is not species slot " << slot << " at column "
                 << x;
}

// AND THE MASK LEAVES AS GREY WITH AN OPAQUE ALPHA. Both halves are load-bearing and neither is a
// preference: grey so the picture opens as the value it is rather than as a red channel, and opaque so no
// tool reads the half of the mask that REMOVES cloud — the dark half — as transparency and composites it
// away before the artist ever sees it.
TEST( CloudLayoutTextures, TheExportedMaskIsGreyWithAnOpaqueAlphaAndCarriesTheStoredByte )
{
    CloudLayoutCanvas canvas = DistinctCanvas();
    ASSERT_TRUE( SetCloudLayoutCanvasMask( canvas, true ) );

    for ( uint32_t x = 0; x < kSide; ++x )
        canvas.Mask[static_cast<size_t>( 5u ) * kSide + x] = static_cast<unsigned char>( 3u + 7u * x );

    auto picture = EncodeCloudLayoutCanvasMaskToImage( canvas );
    ASSERT_TRUE( picture ) << picture.GetError();

    for ( uint32_t x = 0; x < kSide; ++x )
    {
        const size_t        at    = ( static_cast<size_t>( 5u ) * kSide + x ) * 4u;
        const unsigned char value = static_cast<unsigned char>( 3u + 7u * x );

        ASSERT_EQ( picture.GetValue().Pixels[at + 0], value ) << "the mask's byte was altered on export";
        ASSERT_EQ( picture.GetValue().Pixels[at + 1], value ) << "green does not carry the mask, so the "
                                                                 "picture opens tinted rather than grey";
        ASSERT_EQ( picture.GetValue().Pixels[at + 2], value ) << "blue does not carry the mask";
        ASSERT_EQ( picture.GetValue().Pixels[at + 3], 255u )
             << "the exported mask is not opaque, so the part of it that removes cloud reads as "
                "transparency in every tool that composites";
    }
}

// A PAINTING WITH NO MASK REFUSES TO EXPORT ONE rather than writing a neutral file. A neutral picture
// would claim a table this layout does not have, and the artist who imported it back would be given a mask
// that costs a fetch per cell to add exactly zero.
TEST( CloudLayoutTextures, ACanvasWithNoMaskRefusesToExportOne )
{
    const CloudLayoutCanvas canvas = DistinctCanvas();
    ASSERT_FALSE( canvas.HasMask() );

    EXPECT_FALSE( EncodeCloudLayoutCanvasMaskToImage( canvas ) )
         << "a painting with no mask exported one anyway, so a table it does not carry now exists as a file";
}

// =====================================================================================================
// THE ROUND TRIP, per table and separately
// =====================================================================================================

// EACH TABLE GOES OUT AND COMES BACK BYTE FOR BYTE, and the two trips are independent: exporting the
// pattern and re-importing it must not touch the mask, and the reverse. That independence is the whole
// point of two textures, so it is asserted rather than assumed.
TEST( CloudLayoutTextures, EachTableRoundTripsThroughItsOwnPictureWithoutTouchingTheOther )
{
    CloudLayoutCanvas original = DistinctCanvas();
    ASSERT_TRUE( SetCloudLayoutCanvasMask( original, true ) );
    for ( uint32_t x = 0; x < kSide; ++x )
        original.Mask[static_cast<size_t>( 9u ) * kSide + x] = static_cast<unsigned char>( 200u - 3u * x );

    auto patternPicture = EncodeCloudLayoutCanvasPatternToImage( original );
    auto maskPicture    = EncodeCloudLayoutCanvasMaskToImage( original );
    ASSERT_TRUE( patternPicture ) << patternPicture.GetError();
    ASSERT_TRUE( maskPicture ) << maskPicture.GetError();

    const uint32_t identity[kCloudLayoutChannels] = { 0u, 1u, 2u, 3u };

    // Pattern first, into a canvas that already carries the mask: the mask must survive untouched.
    CloudLayoutCanvas rebuilt = original;
    std::fill( rebuilt.Pattern.begin(), rebuilt.Pattern.end(), 0u );
    ASSERT_TRUE( SetCloudLayoutCanvasPatternFromImage( rebuilt, patternPicture.GetValue().Pixels, kSide, kSide,
                                                       identity ) );
    EXPECT_EQ( rebuilt.Pattern, original.Pattern ) << "the pattern did not survive its own round trip";
    EXPECT_EQ( rebuilt.Mask, original.Mask )
         << "importing a pattern changed the mask, so the two inputs are not independent after all";

    // Then the mask, into a canvas whose pattern is already right: the pattern must survive untouched.
    std::fill( rebuilt.Mask.begin(), rebuilt.Mask.end(), kCloudLayoutMaskNeutral );
    ASSERT_TRUE( SetCloudLayoutCanvasMaskFromImage( rebuilt, maskPicture.GetValue().Pixels, kSide, kSide, 0u ) );
    EXPECT_EQ( rebuilt.Mask, original.Mask ) << "the mask did not survive its own round trip";
    EXPECT_EQ( rebuilt.Pattern, original.Pattern )
         << "importing a mask changed the pattern, so the two inputs are not independent after all";
}

// THE TWO TABLES OF ONE LAYOUT SHARE A RESOLUTION, and it is refused rather than resampled — from BOTH
// sides, because a rule enforced in one direction is a rule an artist meets by doing it the other way
// round.
TEST( CloudLayoutTextures, ATableOfTheWrongSideIsRefusedFromEitherDirection )
{
    const uint32_t identity[kCloudLayoutChannels] = { 0u, 1u, 2u, 3u };

    const std::vector<unsigned char> small( static_cast<size_t>( 16u ) * 16u * 4u, 128u );

    CloudLayoutCanvas withMask = DistinctCanvas();
    ASSERT_TRUE( SetCloudLayoutCanvasMask( withMask, true ) );
    EXPECT_FALSE( SetCloudLayoutCanvasPatternFromImage( withMask, small, 16u, 16u, identity ) )
         << "a 16-square pattern was accepted onto a canvas whose mask is 32 square, so one of the two "
            "tables would be sampled at a resolution nobody authored";

    CloudLayoutCanvas withPattern = DistinctCanvas();
    EXPECT_FALSE( SetCloudLayoutCanvasMaskFromImage( withPattern, small, 16u, 16u, 0u ) )
         << "a 16-square mask was accepted onto a 32-square pattern";

    // AND A NON-SQUARE PICTURE IS REFUSED IN BOTH SLOTS, for the reason the file's own note gives: the
    // painting tiles the world on a square period, and resampling is an opinion about somebody's art.
    const std::vector<unsigned char> oblong( static_cast<size_t>( 32u ) * 16u * 4u, 128u );
    CloudLayoutCanvas                empty;
    EXPECT_FALSE( SetCloudLayoutCanvasPatternFromImage( empty, oblong, 32u, 16u, identity ) );
    EXPECT_FALSE( SetCloudLayoutCanvasMaskFromImage( empty, oblong, 32u, 16u, 0u ) );
}

// A MASK BROUGHT IN ON ITS OWN MAKES A MASK-ONLY PAINTING, and opening it again does not invent a
// pattern for it. Both halves matter: the first is what "two separate textures" means when only one of
// them is authored — Unreal's mask parameter with its pattern slot left alone — and the second is a
// silent-change trap. Filling the canvas from a blank one and copying in what the layout has would give
// such a file a flat pattern table it never carried, and pressing Bake would write that table to disk: a
// file that changed because somebody looked at it.
TEST( CloudLayoutTextures, AMaskOnItsOwnStaysAMaskOnlyPaintingThroughEveryTrip )
{
    const size_t               texels = static_cast<size_t>( kSide ) * kSide;
    std::vector<unsigned char> picture( texels * 4u, 255u );
    for ( size_t t = 0; t < texels; ++t )
    {
        const unsigned char value = static_cast<unsigned char>( 40u + ( t % 100u ) );
        picture[t * 4u + 0]       = value;
        picture[t * 4u + 1]       = value;
        picture[t * 4u + 2]       = value;
    }

    CloudLayoutCanvas canvas;
    ASSERT_TRUE( SetCloudLayoutCanvasMaskFromImage( canvas, picture, kSide, kSide, 0u ) );

    EXPECT_EQ( canvas.Side, kSide );
    EXPECT_TRUE( canvas.Pattern.empty() )
         << "a mask imported on its own invented a pattern table, so a mask-only painting cannot be "
            "expressed and every such file grows a plane it did not have";
    ASSERT_TRUE( canvas.HasMask() );

    auto made = MakeCloudLayoutFromCanvas( canvas );
    ASSERT_TRUE( made ) << made.GetError();
    EXPECT_FALSE( made.GetValue().HasPattern() ) << "the layout gained a pattern table on the way to disk";
    ASSERT_TRUE( made.GetValue().HasMask() );

    // AND BACK ONTO A CANVAS, which is the trip the panel makes when the document is opened.
    auto reopened = MakeCloudLayoutCanvasFromLayout( made.GetValue() );
    ASSERT_TRUE( reopened ) << reopened.GetError();
    EXPECT_TRUE( reopened.GetValue().Pattern.empty() )
         << "opening a mask-only painting gave it a pattern, so saving it again would write a table the "
            "file never carried";
    EXPECT_EQ( reopened.GetValue().Mask, canvas.Mask );
}

// =====================================================================================================
// WHAT THE TWO INPUTS DO TO A SKY
// =====================================================================================================

// THE MASK'S NEUTRAL CHANGES NOTHING, EXACTLY. This is the claim the whole signed convention rests on, and
// "exactly" is the word that matters: an off-by-one neutral would move every cell in the sky by a fraction
// nobody could see in a frame and every scene's Coverage mapping with it (D-20). Asserted with FLOAT_EQ
// against the sky the same parameters make with no mask at all.
TEST( CloudLayoutTextures, ANeutralMaskIsExactlyTheSkyWithNoMaskAtAll )
{
    CloudProceduralFieldParams bare = Params();

    CloudLayoutCanvas neutral = DistinctCanvas( 64u );
    ASSERT_TRUE( SetCloudLayoutCanvasMask( neutral, true ) );
    auto asLayout = MakeCloudLayoutFromCanvas( neutral );
    ASSERT_TRUE( asLayout ) << asLayout.GetError();
    ASSERT_TRUE( asLayout.GetValue().HasMask() );

    CloudProceduralFieldParams masked = bare;
    masked.MaskSource                 = Share( asLayout.ExtractValue() );

    const std::vector<float> without = CoverageMap( bare );
    const std::vector<float> with    = CoverageMap( masked );

    ASSERT_FALSE( without.empty() );
    ASSERT_EQ( without.size(), with.size() );

    for ( size_t cell = 0; cell < without.size(); ++cell )
        ASSERT_FLOAT_EQ( with[cell], without[cell] )
             << "cell " << cell << ": a mask flooded with the neutral " << int( kCloudLayoutMaskNeutral )
             << " moved the sky, so 'mid-grey does nothing' is false and every scene's Coverage means "
                "something slightly different the moment a mask is bound";
}

// AND IT ADDS AND REMOVES — BOTH, from one table, which is what makes it a mask rather than a second
// coverage slider.
//
// MEASURED ON UNIFORM MASKS AND NOT ON A PAINTED FIGURE, deliberately. A figure would make this test also
// a test of which world position a texel lands on — the preview's span is centred on the origin, so the
// map's left half is the painting's right half, and the first draft of this test failed on exactly that
// and looked like a defect in the mask. The claim here is about SIGN, so the fixture is the one that has
// no geometry in it at all: flood the mask above neutral and every cell must rise, flood it below and
// every cell must fall. Where a painted value lands is CloudPlacementSpectrum's subject and is asserted
// there.
TEST( CloudLayoutTextures, TheMaskAddsAboveNeutralAndRemovesBelowIt )
{
    constexpr uint32_t kMapSide = 64u;

    const CloudProceduralFieldParams bare    = Params();
    const std::vector<float>         without = CoverageMap( bare );
    ASSERT_FALSE( without.empty() );

    // NEITHER END SATURATES THE CLAMP, and that is what the strength is for. A full-strength flood of 255
    // drives every cell to 1 and 0 drives every cell to 0, at which point the map says the clamp works
    // rather than that the mask does. At a fifth the excursion is 0.2 about a slider of 0.5.
    const auto flooded = [&]( unsigned char value )
    {
        CloudLayoutCanvas canvas = DistinctCanvas( kMapSide );
        EXPECT_TRUE( SetCloudLayoutCanvasMask( canvas, true ) );
        std::fill( canvas.Mask.begin(), canvas.Mask.end(), value );

        auto made = MakeCloudLayoutFromCanvas( canvas );
        EXPECT_TRUE( made ) << ( made ? "" : made.GetError() );

        CloudProceduralFieldParams params   = Params();
        params.MaskSource                   = Share( made.ExtractValue() );
        params.LayoutPlacement.MaskStrength = 0.2f;
        return CoverageMap( params );
    };

    const std::vector<float> brighter = flooded( 255u );
    const std::vector<float> darker   = flooded( 0u );

    ASSERT_EQ( brighter.size(), without.size() );
    ASSERT_EQ( darker.size(), without.size() );

    for ( size_t cell = 0; cell < without.size(); ++cell )
    {
        ASSERT_GT( brighter[cell], without[cell] )
             << "cell " << cell << ": a mask flooded ABOVE neutral did not add cloud";
        ASSERT_LT( darker[cell], without[cell] )
             << "cell " << cell << ": a mask flooded BELOW neutral did not remove cloud";
    }

    // AND THE TWO DIRECTIONS COME OUT OF ONE TABLE, which is the sentence "adds or removes" actually
    // means: a mask painted bright in one place and dark in another moves the sky both ways at once.
    CloudLayoutCanvas split = DistinctCanvas( kMapSide );
    ASSERT_TRUE( SetCloudLayoutCanvasMask( split, true ) );
    for ( uint32_t y = 0; y < kMapSide; ++y )
        for ( uint32_t x = 0; x < kMapSide; ++x )
            split.Mask[static_cast<size_t>( y ) * kMapSide + x] = x < kMapSide / 2u ? 255u : 0u;

    auto made = MakeCloudLayoutFromCanvas( split );
    ASSERT_TRUE( made ) << made.GetError();

    CloudProceduralFieldParams mixed   = Params();
    mixed.MaskSource                   = Share( made.ExtractValue() );
    mixed.LayoutPlacement.MaskStrength = 0.2f;

    const std::vector<float> both = CoverageMap( mixed );
    ASSERT_EQ( both.size(), without.size() );

    int raised  = 0;
    int lowered = 0;
    for ( size_t cell = 0; cell < without.size(); ++cell )
    {
        raised += both[cell] > without[cell] ? 1 : 0;
        lowered += both[cell] < without[cell] ? 1 : 0;
    }

    EXPECT_GT( raised, 0 ) << "no cell gained cloud under a mask that is bright over half the sky";
    EXPECT_GT( lowered, 0 ) << "no cell lost cloud under a mask that is dark over half the sky";
}

// THE TWO INPUTS ARE READ FROM THEIR OWN SLOTS AND NOWHERE ELSE. A layout bound to the pattern input must
// contribute its pattern and NOT its mask, and the reverse — otherwise "two inputs" is a label on one, and
// an artist who bound a painting to the mask slot alone would silently get its placement too.
TEST( CloudLayoutTextures, EachInputReadsOnlyItsOwnTableOutOfWhateverIsBoundToIt )
{
    constexpr uint32_t kMapSide = 64u;

    // ONE painting carrying BOTH tables, each saying something different, so a slot reading the wrong one
    // is a different sky rather than the same one.
    CloudLayoutCanvas both = DistinctCanvas( kMapSide );
    ASSERT_TRUE( SetCloudLayoutCanvasMask( both, true ) );
    for ( uint32_t y = 0; y < kMapSide; ++y )
        for ( uint32_t x = 0; x < kMapSide; ++x )
        {
            const size_t at                              = static_cast<size_t>( y ) * kMapSide + x;
            both.Pattern[at * kCloudLayoutChannels + 0u] = x < kMapSide / 2u ? 255u : 0u;
            both.Mask[at]                                = y < kMapSide / 2u ? 255u : 0u;
        }

    auto made = MakeCloudLayoutFromCanvas( both );
    ASSERT_TRUE( made ) << made.GetError();
    const std::shared_ptr<const CloudLayoutData> painting = Share( made.ExtractValue() );

    // A GENTLE MASK, for the reason the test above gives: at full strength a black-and-white mask drives
    // every cell to one clamp or the other, and a clamped cell cannot show whether the pattern reached it.
    // That is the §PT pair of byte-identical frames, and it would make the last assertion below fail for a
    // reason that has nothing to do with the two inputs.
    CloudProceduralFieldParams patternOnly   = Params();
    patternOnly.PatternSource                = painting;
    patternOnly.LayoutPlacement.MaskStrength = 0.2f;

    CloudProceduralFieldParams maskOnly   = Params();
    maskOnly.MaskSource                   = painting;
    maskOnly.LayoutPlacement.MaskStrength = 0.2f;

    CloudProceduralFieldParams together   = Params();
    together.PatternSource                = painting;
    together.MaskSource                   = painting;
    together.LayoutPlacement.MaskStrength = 0.2f;

    const std::vector<float> pattern = CoverageMap( patternOnly );
    const std::vector<float> mask    = CoverageMap( maskOnly );
    const std::vector<float> all     = CoverageMap( together );

    ASSERT_FALSE( pattern.empty() );
    ASSERT_EQ( pattern.size(), mask.size() );
    ASSERT_EQ( pattern.size(), all.size() );

    // The pattern's figure varies along x and the mask's along y, so a slot reading the wrong table shows
    // its variation on the wrong axis. Three maps that are all different is the weakest claim that
    // distinguishes "two inputs" from "one input read twice".
    EXPECT_NE( pattern, mask ) << "binding one painting to the pattern input and to the mask input made the "
                                  "same sky, so one of the two slots is reading the other's table";
    EXPECT_NE( pattern, all ) << "adding the mask input on top of the pattern changed nothing";
    EXPECT_NE( mask, all ) << "adding the pattern input on top of the mask changed nothing";
}

// A LAYOUT DROPPED INTO THE SLOT IT HAS NO TABLE FOR IS NAMED, not quietly ignored. That is the dead-slot
// shape the contract's §1.3 forbids arrived at from the far side: the artist fills the input, the sky does
// not move, and nothing anywhere says why.
TEST( CloudLayoutTextures, ASlotFedALayoutWithoutItsTableRefusesByName )
{
    CloudLayoutCanvas patternOnly = DistinctCanvas( 64u );
    auto              made        = MakeCloudLayoutFromCanvas( patternOnly );
    ASSERT_TRUE( made ) << made.GetError();
    ASSERT_FALSE( made.GetValue().HasMask() );

    CloudProceduralFieldParams params = Params();
    params.MaskSource                 = Share( made.ExtractValue() );

    const auto verdict = ValidateCloudProceduralLayoutTable( params, CloudLayoutTable::Mask );
    EXPECT_FALSE( verdict ) << "a painting with no mask table was accepted on the mask input, so that slot "
                               "decides nothing and says nothing";
    EXPECT_NE( verdict.GetError().find( "mask" ), std::string::npos )
         << "the refusal does not name the slot it is about: " << verdict.GetError();

    // AND THE OTHER SLOT IS UNAFFECTED, which is the whole reason the check is per table: a wrong mask
    // must not cost the artist their placement.
    EXPECT_TRUE( ValidateCloudProceduralLayoutTable( params, CloudLayoutTable::Pattern ) )
         << "an empty pattern input was reported as a fault";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
