// The cloud TYPE: its file format, and the SHIPPED LIBRARY that format carries.
//
// Two halves, and the second is the one that matters most. The first is ordinary format work — a
// round trip, and a refusal for every number the generator cannot honour. The second opens the nine
// `.decloudtype` files this task ships and asserts things about their CONTENTS: that the altitudes are
// meteorology rather than taste, that the four T0 inherited are unchanged to the digit, and that the
// built-in default an empty slot resolves to is the same row as the file that claims to be it.
//
// THAT LAST ONE IS THE POINT OF THE SUITE. "A preset table against the saved scenes" is the eighth row of
// the table in DEV_CONTRACT.md §2.3.1 — two places obliged to agree, each correct on its own, and the
// symptom of their disagreeing is a sky that is subtly not the one anybody authored. The built-in row
// lives in C++ because a scene with no type in its slot still has to render; the file lives on disk
// because an artist has to be able to open it. There is no way to make them one thing, so they are made
// one number at a time, here.
//
// The library is read from DISK rather than embedded, deliberately: an embedded copy would be a third
// statement of the same numbers and would pass while the shipped files were broken.

#include <Common/Json/Json.hpp>
#include "CloudScheduleReference.hpp"

#include <Engine/Assets/CloudProceduralVolume.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>

// The BAKE's half of the layer's shell, so that it and the packer's half can be asserted equal in one
// place. They are two independent calls of CloudTypeSetEnvelopeKm and nothing but a test makes them agree.
#include <Engine/Graphic/Clouds/CloudMaterialBake.hpp>

#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/CloudLayout.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>

// The LAYER's Detail Strength, for the one relation that is between the library and the layer: the cut's
// depth is their product and it is clamped. PROPERTY expands to nothing, so this costs no reflection.
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>

#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Assets;
using Desert::Graphic::CloudTypeBaseKm;
using Desert::Graphic::CloudTypeShape;
using Desert::Graphic::CloudTypeTopKm;

namespace
{
    // A shape every field of which is legal, so a test that breaks ONE field is testing that field.
    CloudTypeShape LegalShape()
    {
        return CloudTypeShape{
             /* BaseAltitudeKm   */ 1.00f,
             /* TopAltitudeKm    */ 3.00f,
             /* EdgeTopFraction  */ 0.40f,
             /* BaseRampFraction */ 0.10f,
             /* Profile          */ Desert::Graphic::CloudProfileFromTaper( 0.40f ),
             /* AnvilAltitudeKm  */ 0.00f,
             /* AnvilThicknessKm */ 0.00f,
             /* AnvilStrength    */ 0.00f,
             /* DetailCharacter  */ 0.60f,
             /* DetailFactor     */ 1.00f,
             /* DensityFactor    */ 1.00f,
             /* ExtinctionFactor */ 1.00f,
             /* PlacementScale      */ 1.00f,
             /* PlacementAnisotropy */ 1.00f,
        };
    }

    CloudTypeData LegalData()
    {
        CloudTypeData data;
        data.DisplayName   = "Test type";
        data.Notes         = "Written by the round-trip test.";
        data.Shape         = LegalShape();
        return data;
    }

    // WHERE THE SHIPPED LIBRARY IS, found by walking up from wherever the test binary was started. The
    // repository runs its suites from the workspace root (scripts/MacOS/RunTests.sh), but a developer
    // running one binary by hand from build/Bin/Tests/Debug is the normal case and a suite that fails for
    // them is a suite they will stop running.
    std::filesystem::path LibraryDirectory()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 6; ++up )
        {
            const std::filesystem::path candidate = here / "Editor" / "Resources" / "Assets" / "Clouds" / "Types";
            std::error_code             ec;
            if ( std::filesystem::is_directory( candidate, ec ) )
                return candidate;
            if ( !here.has_parent_path() )
                break;
            here = here.parent_path();
        }
        return {};
    }

    // Opens one shipped preset by name, or FAILS. Not skipped: a library that is not there is exactly the
    // failure this suite exists to catch — the migration writes these names into every scene it raises.
    CloudTypeData LoadShipped( const char* name )
    {
        const std::filesystem::path dir = LibraryDirectory();
        EXPECT_FALSE( dir.empty() ) << "Editor/Resources/Assets/Clouds/Types was not found from "
                                    << std::filesystem::current_path()
                                    << " or any of its six parents — the shipped cloud type library is "
                                       "missing, and every scene raised by the v4 -> v5 migration names it";

        const std::filesystem::path path = dir / ( std::string( name ) + kCloudTypeExtension );

        std::ifstream file( path );
        EXPECT_TRUE( file.good() ) << "shipped cloud type '" << path.string() << "' could not be opened";

        std::stringstream buffer;
        buffer << file.rdbuf();

        auto parsed = ParseCloudType( buffer.str() );
        EXPECT_TRUE( parsed ) << "shipped cloud type '" << path.string()
                              << "' does not parse: " << ( parsed ? std::string{} : parsed.GetError() );
        return parsed ? parsed.ExtractValue() : CloudTypeData{};
    }

    // ------------------------------------------------------------------------------------------------
    // WHAT A TYPE ACTUALLY PUTS IN THE SKY, measured off the PRODUCER
    // ------------------------------------------------------------------------------------------------
    //
    // These three used to evaluate Graphic::CloudProfileCurve — the parametric curve behind the profile
    // table. There is no curve and no table: the profile is the normalised distance field of a pile of
    // lumps the generator places, so a helper that still evaluated the curve would be asserting
    // meteorology about arithmetic nothing renders.
    //
    // They place the lumps this type would place, with the same function the bake calls, and read the
    // field up a vertical line through the tallest cluster. That is a STRONGER anchor than the curve was:
    // it measures where the material ends up rather than where a generator intended to put it.

    /// The bake parameters for one type on its own, at the layer's shipped lattice.
    Desert::Assets::CloudProceduralFieldParams TypeParams( const CloudTypeShape& shape )
    {
        Desert::Assets::CloudProceduralFieldParams params;

        const float latticeKm = 3.0f * std::max( shape.PlacementScale, 1e-3f );

        params.RegionSizeKm      = std::max( latticeKm * 6.0f, 16.1f );
        params.LayerBottomKm     = CloudTypeBaseKm( shape );
        params.LayerThicknessKm  = std::max( CloudTypeTopKm( shape ) - params.LayerBottomKm, 0.001f );
        params.BlendRadiusKm     = 0.02f * latticeKm;
        params.ProfileDepthKm    = 0.12f * latticeKm;
        params.Coverage          = 1.0f;
        params.CoverageContrast  = 1.0f;
        params.Seed              = 1u;
        params.WindAxis          = glm::vec2( 1.0f, 0.0f );
        params.ResolvableChordKm = Desert::Graphic::CloudFinestResolvableChordKm( 256.0f );

        Desert::Assets::CloudProceduralSpecies species;
        species.Shape      = shape;
        species.CellKm     = latticeKm;
        species.Anisotropy = std::max( shape.PlacementAnisotropy, 1e-3f );
        params.Species.push_back( species );

        return params;
    }

    /// The tallest cluster this type places in one region, and the lumps of that region.
    struct TypeColumn
    {
        Desert::Assets::CloudProceduralFieldParams      Params;
        std::vector<Desert::Assets::CloudModellingBlob> Blobs;
        glm::vec3                                       TallestKm{ 0.0f };
    };

    const TypeColumn& ColumnOf( const CloudTypeShape& shape )
    {
        // Memoized on the shape's bytes: nine types, three helpers each, and a bake of the lump list is
        // the expensive part. It changes no answer — the generator is a pure function.
        static std::map<std::string, TypeColumn> cache;

        const std::string key( reinterpret_cast<const char*>( &shape ), sizeof( CloudTypeShape ) );

        const auto it = cache.find( key );
        if ( it != cache.end() )
            return it->second;

        TypeColumn column;
        column.Params = TypeParams( shape );

        const glm::vec2 origin = Desert::Assets::CloudProceduralRegionOriginKm( column.Params, 0.0f, 0.0f );

        column.Blobs = Desert::Assets::GenerateCloudProceduralBlobs( column.Params, 0u, origin );

        if ( !column.Blobs.empty() )
        {
            column.TallestKm = column.Blobs.front().CentreKm;
            for ( const Desert::Assets::CloudModellingBlob& blob : column.Blobs )
            {
                if ( blob.CentreKm.y > column.TallestKm.y )
                    column.TallestKm = blob.CentreKm;
            }
        }

        return cache.emplace( key, std::move( column ) ).first->second;
    }

    /// The profile up the vertical line through that cluster, sampled at @p altitudeKm.
    float ProfileAt( const CloudTypeShape& shape, float altitudeKm )
    {
        const TypeColumn& column = ColumnOf( shape );
        if ( column.Blobs.empty() )
            return 0.0f;

        return Desert::Assets::EvaluateCloudProceduralProfile(
             column.Params, column.Blobs, glm::vec3( column.TallestKm.x, altitudeKm, column.TallestKm.z ) );
    }

    float ProfileBaseKm( const CloudTypeShape& shape )
    {
        const float top = CloudTypeTopKm( shape );
        for ( int i = 0; i <= 2000; ++i )
        {
            const float altitudeKm = top * static_cast<float>( i ) / 2000.0f;
            if ( ProfileAt( shape, altitudeKm ) > 0.0f )
                return altitudeKm;
        }
        return top;
    }

    float ProfileTopKm( const CloudTypeShape& shape )
    {
        const float top = CloudTypeTopKm( shape );
        for ( int i = 2000; i >= 0; --i )
        {
            const float altitudeKm = top * static_cast<float>( i ) / 2000.0f;
            if ( ProfileAt( shape, altitudeKm ) > 0.0f )
                return altitudeKm;
        }
        return 0.0f;
    }

    // How much matter a column of this type holds at the core of a patch: the profile integrated over
    // altitude, weighted by how opaque the type's own matter is. Two types with the same integral are two
    // types that will not be told apart in a frame, whatever their names.
    float ColumnOpacity( const CloudTypeShape& shape )
    {
        const float baseKm = CloudTypeBaseKm( shape );
        const float topKm  = CloudTypeTopKm( shape );
        const float step   = ( topKm - baseKm ) / 1000.0f;

        float total = 0.0f;
        for ( int i = 0; i < 1000; ++i )
            total += ProfileAt( shape, baseKm + ( static_cast<float>( i ) + 0.5f ) * step ) * step;

        return total * shape.DensityFactor * shape.ExtinctionFactor;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The format.
// ---------------------------------------------------------------------------------------------------

TEST( CloudTypeFormat, ATypeSurvivesBeingWrittenAndReadBack )
{
    const CloudTypeData original = LegalData();

    auto parsed = ParseCloudType( WriteCloudType( original ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();

    const CloudTypeData round = parsed.ExtractValue();

    EXPECT_EQ( round.DisplayName, original.DisplayName );
    EXPECT_EQ( round.Notes, original.Notes );
    EXPECT_EQ( round.NoiseVolume, original.NoiseVolume );
    // The writer stamped a header: this build's kind and version, and a GUID a second write keeps.
    ASSERT_TRUE( round.Header.has_value() );
    EXPECT_EQ( round.Header->Kind, "CloudType" );
    EXPECT_EQ( Desert::Assets::StatedVersion( round.Header, Desert::Assets::kCloudTypeSchemaTag ),
               kCloudTypeFormatVersion );
    auto again = ParseCloudType( WriteCloudType( round ) );
    ASSERT_TRUE( again ) << again.GetError();
    EXPECT_EQ( again.GetValue().Header->Guid, round.Header->Guid ) << "a rewrite minted a second identity";

    // Every one of the twelve, named individually rather than compared as bytes: a field that stopped
    // being written would otherwise be reported as "the structs differ" and leave the reader to find which.
    EXPECT_FLOAT_EQ( round.Shape.BaseAltitudeKm, original.Shape.BaseAltitudeKm );
    EXPECT_FLOAT_EQ( round.Shape.TopAltitudeKm, original.Shape.TopAltitudeKm );
    EXPECT_FLOAT_EQ( round.Shape.EdgeTopFraction, original.Shape.EdgeTopFraction );
    EXPECT_FLOAT_EQ( round.Shape.BaseRampFraction, original.Shape.BaseRampFraction );
    // THE CURVE SAMPLE BY SAMPLE, and not as a struct comparison: a writer that dropped the tail of the
    // row would otherwise report "the shapes differ" and leave sixteen numbers to search.
    for ( uint32_t i = 0; i < Desert::Graphic::kCloudProfileSamples; ++i )
        EXPECT_FLOAT_EQ( round.Shape.Profile.HalfWidth[i], original.Shape.Profile.HalfWidth[i] ) << i;
    EXPECT_FLOAT_EQ( round.Shape.AnvilAltitudeKm, original.Shape.AnvilAltitudeKm );
    EXPECT_FLOAT_EQ( round.Shape.AnvilThicknessKm, original.Shape.AnvilThicknessKm );
    EXPECT_FLOAT_EQ( round.Shape.AnvilStrength, original.Shape.AnvilStrength );
    EXPECT_FLOAT_EQ( round.Shape.DetailCharacter, original.Shape.DetailCharacter );
    EXPECT_FLOAT_EQ( round.Shape.DetailFactor, original.Shape.DetailFactor );
    EXPECT_FLOAT_EQ( round.Shape.DensityFactor, original.Shape.DensityFactor );
    EXPECT_FLOAT_EQ( round.Shape.ExtinctionFactor, original.Shape.ExtinctionFactor );
}

TEST( CloudTypeFormat, TheOptionalFieldsAreOptionalAndTheShapeIsNot )
{
    // A file an artist wrote by hand, with nothing in it but the numbers that have no answer.
    const std::string minimal =
         R"({"Header":{"Kind":"CloudType","Guid":"0123456789abcdef0123456789abcdef","Versions":{"CLTY":5},"Dependencies":[]},"Shape":{
        "BaseAltitudeKm":1.0,"TopAltitudeKm":3.0,"EdgeTopFraction":0.4,"BaseRampFraction":0.1,
        "Profile":{"HalfWidth":[0.62,0.60120887,0.5827022,0.56448,0.5465422,0.5288889,0.51152,0.49443555,0.47763556,0.46112,0.4448889,0.42894223,0.41328,0.39790222,0.3828089,0.368]},"AnvilAltitudeKm":0.0,"AnvilThicknessKm":0.0,"AnvilStrength":0.0,
        "DetailCharacter":0.6,"DetailFactor":1.0,"DensityFactor":1.0,"ExtinctionFactor":1.0,
        "PlacementScale":1.0,"PlacementAnisotropy":1.0}})";

    auto parsed = ParseCloudType( minimal );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_FALSE( parsed.GetValue().DisplayName.has_value() );
    EXPECT_FALSE( parsed.GetValue().NoiseVolume.has_value() );

    // And the other way round: a shape with a field MISSING is refused rather than defaulted, because a
    // number nobody wrote is not a number anybody chose.
    const std::string incomplete =
         R"({"Header":{"Kind":"CloudType","Guid":"0123456789abcdef0123456789abcdef","Versions":{"CLTY":5},"Dependencies":[]},"Shape":{
        "BaseAltitudeKm":1.0,"TopAltitudeKm":3.0,"EdgeTopFraction":0.4,"BaseRampFraction":0.1,
        "Profile":{"HalfWidth":[0.62,0.60120887,0.5827022,0.56448,0.5465422,0.5288889,0.51152,0.49443555,0.47763556,0.46112,0.4448889,0.42894223,0.41328,0.39790222,0.3828089,0.368]},"AnvilAltitudeKm":0.0,"AnvilThicknessKm":0.0,"AnvilStrength":0.0,
        "DetailCharacter":0.6,"DetailFactor":1.0,"DensityFactor":1.0,
        "PlacementScale":1.0,"PlacementAnisotropy":1.0}})";
    EXPECT_FALSE( ParseCloudType( incomplete ) ) << "a shape missing ExtinctionFactor was accepted";

    // A VERSION-1 FILE IS REFUSED, NOT FILLED IN. It carries the twelve numbers T1 shipped and neither of
    // the two T3 added, and a fibrous cirrus and a round-patched one are different KINDS of cloud — so
    // guessing the missing pair would render a sky the file does not describe while claiming it does. The
    // nine shipped files were rewritten in the same commit, which is what makes the loud refusal
    // affordable (§4.5).
    const std::string versionOne = R"({"FormatVersion":1,"Shape":{
        "BaseAltitudeKm":1.0,"TopAltitudeKm":3.0,"EdgeTopFraction":0.4,"BaseRampFraction":0.1,
        "Profile":{"HalfWidth":[0.62,0.60120887,0.5827022,0.56448,0.5465422,0.5288889,0.51152,0.49443555,0.47763556,0.46112,0.4448889,0.42894223,0.41328,0.39790222,0.3828089,0.368]},"AnvilAltitudeKm":0.0,"AnvilThicknessKm":0.0,"AnvilStrength":0.0,
        "DetailCharacter":0.6,"DetailFactor":1.0,"DensityFactor":1.0,"ExtinctionFactor":1.0}})";

    const auto refused = ParseCloudType( versionOne );
    ASSERT_FALSE( refused ) << "a version-1 file was read as if it were a version-3 one";
    EXPECT_NE( refused.GetError().find( "version" ), std::string::npos )
         << "the refusal does not say the format version is the problem: " << refused.GetError();

    // AND A VERSION-2 FILE THE SAME WAY, which is the refusal Р2 added and the one that matters most,
    // because a version-2 file is not obviously broken: it is a real, complete, legal type as of last
    // week. What it carries is `TopTaper`, a single number where the reader now wants a curve. The engine
    // COULD synthesise that curve — `Graphic::CloudProfileFromTaper` is exactly that function and it is
    // what rewrote the shipped library — and doing it here is precisely what DEV_CONTRACT.md §4 forbids:
    // it would leave two file layouts alive in the reader for ever, and the older one would be the one
    // nobody tests. The conversion happened once, in the commit, in the files.
    const std::string versionTwo = R"({"FormatVersion":2,"Shape":{
        "BaseAltitudeKm":1.0,"TopAltitudeKm":3.0,"EdgeTopFraction":0.4,"BaseRampFraction":0.1,
        "TopTaper":0.4,"AnvilAltitudeKm":0.0,"AnvilThicknessKm":0.0,"AnvilStrength":0.0,
        "DetailCharacter":0.6,"DetailFactor":1.0,"DensityFactor":1.0,"ExtinctionFactor":1.0,
        "PlacementScale":1.0,"PlacementAnisotropy":1.0}})";

    const auto refusedTwo = ParseCloudType( versionTwo );
    ASSERT_FALSE( refusedTwo ) << "a version-2 file was read as if its TopTaper were a profile curve";
    EXPECT_NE( refusedTwo.GetError().find( "version" ), std::string::npos )
         << "the refusal does not say the format version is the problem: " << refusedTwo.GetError();
}

// ---------------------------------------------------------------------------------------------------
// The vertical profile — Р2. What the curve can say, what it may not say, and that the library still
// says what it said before the format moved.
// ---------------------------------------------------------------------------------------------------

TEST( CloudTypeProfile, TheCurveReadsBackTheNumbersItWasAuthoredWith )
{
    const Desert::Graphic::CloudVerticalProfile profile = Desert::Graphic::CloudProfileFromTaper( 0.5f );

    // AT THE SAMPLE POINTS IT IS THE SAMPLES, exactly — not "close to". An interpolator that missed its
    // own knots would make every authored number a suggestion.
    for ( uint32_t i = 0; i < Desert::Graphic::kCloudProfileSamples; ++i )
    {
        const float t = static_cast<float>( i ) / static_cast<float>( Desert::Graphic::kCloudProfileSamples - 1 );
        EXPECT_FLOAT_EQ( Desert::Graphic::CloudProfileHalfWidth( profile, t ), profile.HalfWidth[i] ) << i;
    }

    // CLAMPED AND NOT EXTRAPOLATED outside [0, 1]. The stack's `t` is a curve of the step index and can
    // land a hair outside; a linear extrapolation there is a negative width at one end and an unbounded
    // one at the other, which is a lump nobody authored.
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudProfileHalfWidth( profile, -3.0f ), profile.HalfWidth.front() );
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudProfileHalfWidth( profile, 7.0f ), profile.HalfWidth.back() );

    // AND BETWEEN TWO KNOTS IT IS THE MIDPOINT, because linear is the whole of the promise.
    const float half = 0.5f / static_cast<float>( Desert::Graphic::kCloudProfileSamples - 1 );
    EXPECT_NEAR( Desert::Graphic::CloudProfileHalfWidth( profile, half ),
                 0.5f * ( profile.HalfWidth[0] + profile.HalfWidth[1] ), 1e-6f );
}

TEST( CloudTypeProfile, TheTowerIsAShapeTheOldLawCouldNotReachAtAnySetting )
{
    // THE CAPABILITY CLAIM, ASSERTED RATHER THAN DESCRIBED. Everything Р2 delivers rests on one property
    // of the law it replaced: `(0.62 - 0.16 t) * (1 - 0.5 * taper * t)` is a product of two lines that
    // both FALL over [0, 1], so it is monotone decreasing for every taper in range. A profile whose
    // maximum is in its interior therefore cannot be written as that product by any taper whatsoever —
    // which is why the vertical was the one UE capability this engine did not have, and why a slider
    // could not have been the answer.
    //
    // If this test ever fails, either the tower preset stopped being a tower or the old law was not what
    // this task said it was; both would invalidate the argument for the whole change.
    for ( int step = 0; step <= 100; ++step )
    {
        const Desert::Graphic::CloudVerticalProfile old =
             Desert::Graphic::CloudProfileFromTaper( static_cast<float>( step ) / 100.0f );

        for ( uint32_t i = 1; i < Desert::Graphic::kCloudProfileSamples; ++i )
            ASSERT_LE( old.HalfWidth[i], old.HalfWidth[i - 1] )
                 << "the law Р2 replaced was not monotone at taper " << step / 100.0f << ", sample " << i;
    }

    const Desert::Graphic::CloudVerticalProfile tower = Desert::Graphic::CloudProfileTower();

    uint32_t peak = 0;
    for ( uint32_t i = 1; i < Desert::Graphic::kCloudProfileSamples; ++i )
    {
        if ( tower.HalfWidth[i] > tower.HalfWidth[peak] )
            peak = i;
    }

    EXPECT_GT( peak, 0u ) << "the tower preset's widest point is its base, so it is reachable by a taper";
    EXPECT_LT( peak, Desert::Graphic::kCloudProfileSamples - 1 )
         << "the tower preset's widest point is its top, which is monotone the other way and still not a "
            "shape with an interior maximum";

    // And the deck: flat is not monotone-decreasing-and-falling either, since the old law lost a quarter
    // of its width by the top even at a taper of zero.
    const Desert::Graphic::CloudVerticalProfile deck = Desert::Graphic::CloudProfileFlatDeck();
    EXPECT_FLOAT_EQ( deck.HalfWidth.front(), deck.HalfWidth.back() );
}

TEST( CloudTypeProfile, EveryShippedTypeIsItsOldTaperReExpressedAndNotReAuthored )
{
    // WHAT MAKES THE FORMAT MOVE HONEST. Version 3 is a change of REPRESENTATION, and the claim that goes
    // with it is that the library renders the sky it rendered before. That claim is only worth anything if
    // something checks it, and this is the check: every shipped file's curve must be exactly the sampled
    // closed form of the taper that file carried at version 2. Those tapers are transcribed from the
    // version-2 files as literals — reading them back out of the version-3 files would be asserting the
    // library against itself.
    const std::map<std::string, float> tapersAtVersionTwo = {
         { kCloudTypeStratus, 0.35f },          { kCloudTypeCumulusMediocris, 0.45f },
         { kCloudTypeCumulusCongestus, 0.50f }, { kCloudTypeCumulonimbus, 0.40f },
         { kCloudTypeCumulusHumilis, 0.45f },   { kCloudTypeStratocumulus, 0.35f },
         { kCloudTypeAltocumulus, 0.35f },      { kCloudTypeCirrus, 0.55f },
         { kCloudTypeLenticular, 0.45f },
    };

    for ( const auto& [name, taper] : tapersAtVersionTwo )
    {
        const CloudTypeShape                        shape    = LoadShipped( name.c_str() ).Shape;
        const Desert::Graphic::CloudVerticalProfile expected = Desert::Graphic::CloudProfileFromTaper( taper );

        for ( uint32_t i = 0; i < Desert::Graphic::kCloudProfileSamples; ++i )
            EXPECT_FLOAT_EQ( shape.Profile.HalfWidth[i], expected.HalfWidth[i] ) << name << " sample " << i;
    }
}

TEST( CloudTypeProfile, AProfileThatDrawsNothingIsRefusedRatherThanDrawnAsSpecks )
{
    CloudTypeShape shape = LegalShape();

    // A CURVE OF ALL ZEROES is a type whose every lump would be floored at the march's resolvable chord,
    // so the sky comes out as a field of identical minimum-sized specks — a shape nobody authored,
    // produced by a clamp. §1.4 of the contract: refuse it and say why, rather than substitute quietly.
    shape.Profile.HalfWidth.fill( 0.0f );
    const auto refused = ValidateCloudTypeShape( shape );
    EXPECT_FALSE( refused );
    EXPECT_NE( std::string( refused.GetError() ).find( "widest" ), std::string::npos ) << refused.GetError();

    // A single non-zero sample is enough to be a cloud, because the interpolation carries it either side.
    shape.Profile.HalfWidth[8] = 0.5f;
    EXPECT_TRUE( ValidateCloudTypeShape( shape ) );

    // A NaN anywhere in the row is refused and NAMED BY INDEX. A NaN that reaches the layout is a lump
    // radius of NaN, a body that never renders and nothing in the log.
    shape.Profile.HalfWidth[3] = std::numeric_limits<float>::quiet_NaN();
    const auto nan             = ValidateCloudTypeShape( shape );
    EXPECT_FALSE( nan );
    EXPECT_NE( std::string( nan.GetError() ).find( "[3]" ), std::string::npos ) << nan.GetError();
}

TEST( CloudTypeFormat, EveryRefusalNamesTheNumberThatIsWrong )
{
    EXPECT_FALSE( ParseCloudType( "" ) );
    EXPECT_FALSE( ParseCloudType( "not json at all" ) );
    EXPECT_FALSE( ParseCloudType( R"({"FormatVersion":9,"Shape":{}})" ) );

    // Each of these is one field away from legal, and the message has to say which field. Checked by
    // SUBSTRING rather than by "it failed", because a refusal that does not name the number is the silent
    // fallback §1.4 forbids wearing an error's clothes.
    struct Case
    {
        const char* Field;
        CloudTypeShape ( *Break )( CloudTypeShape );
    };

    const Case cases[] = {
         { "TopAltitudeKm",
           []( CloudTypeShape s )
           {
               s.TopAltitudeKm = s.BaseAltitudeKm;
               return s;
           } },
         { "EdgeTopFraction",
           []( CloudTypeShape s )
           {
               s.EdgeTopFraction = 1.5f;
               return s;
           } },
         { "BaseRampFraction",
           []( CloudTypeShape s )
           {
               s.BaseRampFraction = 0.0f;
               return s;
           } },
         { "Profile.HalfWidth",
           []( CloudTypeShape s )
           {
               s.Profile.HalfWidth[7] = -0.1f;
               return s;
           } },
         { "AnvilThicknessKm",
           []( CloudTypeShape s )
           {
               s.AnvilStrength    = 0.8f;
               s.AnvilThicknessKm = 0.0f;
               return s;
           } },
         { "DetailCharacter",
           []( CloudTypeShape s )
           {
               s.DetailCharacter = 2.0f;
               return s;
           } },
         { "DensityFactor",
           []( CloudTypeShape s )
           {
               s.DensityFactor = -1.0f;
               return s;
           } },
         { "ExtinctionFactor",
           []( CloudTypeShape s )
           {
               s.ExtinctionFactor = 99.0f;
               return s;
           } },
         { "BaseAltitudeKm",
           []( CloudTypeShape s )
           {
               s.BaseAltitudeKm = std::nanf( "" );
               return s;
           } },
         // T3'S TWO. The scale is a DIVISOR in the placement basis, so a zero there is an infinity in a
         // texture coordinate and a sky that is banded black with nothing in any log - which is exactly
         // the class of failure this table exists to refuse at the door.
         { "PlacementScale",
           []( CloudTypeShape s )
           {
               s.PlacementScale = 0.0f;
               return s;
           } },
         { "PlacementAnisotropy",
           []( CloudTypeShape s )
           {
               s.PlacementAnisotropy = 40.0f;
               return s;
           } },
    };

    for ( const Case& c : cases )
    {
        const auto broken = ValidateCloudTypeShape( c.Break( LegalShape() ) );
        ASSERT_FALSE( broken ) << c.Field << " was accepted";
        EXPECT_NE( broken.GetError().find( c.Field ), std::string::npos )
             << "the refusal does not name the field that is wrong: " << broken.GetError();
    }

    // And the legal one is legal, so the eight above are testing the fields and not the function.
    EXPECT_TRUE( ValidateCloudTypeShape( LegalShape() ) );
}

TEST( CloudTypeFormat, TheRelativeDirectoryAgreesWithTheProjectPath )
{
    // TWO STATEMENTS OF ONE DIRECTORY. The migration composes its paths from the relative constant,
    // because it is pure and cannot read a project root; the preloader scans the absolute one. If they
    // ever part company, every scene raised by the migration names a file the scan does not load, and the
    // symptom is a sky that quietly reverts to the built-in default.
    const std::string absolute = Common::Constants::Path::CLOUD_TYPE_PATH.generic_string();
    const std::string relative = kCloudTypeAssetsRelativeDir;

    ASSERT_GE( absolute.size(), relative.size() );
    EXPECT_EQ( absolute.compare( absolute.size() - relative.size(), relative.size(), relative ), 0 )
         << "'" << absolute << "' does not end with '" << relative << "'";

    EXPECT_EQ( CloudTypeAssetRelativePath( kCloudTypeCumulusCongestus ),
               "Clouds/Types/Cumulus_Congestus.decloudtype" );
}

// ---------------------------------------------------------------------------------------------------
// The shipped library — content, read off the disk.
// ---------------------------------------------------------------------------------------------------

TEST( CloudTypeLibrary, TheBuiltInDefaultIsTheShippedCumulusCongestus )
{
    // THE RELATION THIS SUITE EXISTS FOR. An empty slot renders CloudTypeDefaultShape; a scene raised by
    // the v4 -> v5 migration renders Cumulus_Congestus.decloudtype; both are described everywhere as "the
    // default cumulus congestus". Nothing but this test makes that true.
    const CloudTypeShape& builtIn = CloudTypeDefaultShape();
    const CloudTypeShape  shipped = LoadShipped( kCloudTypeCumulusCongestus ).Shape;

    EXPECT_FLOAT_EQ( shipped.BaseAltitudeKm, builtIn.BaseAltitudeKm );
    EXPECT_FLOAT_EQ( shipped.TopAltitudeKm, builtIn.TopAltitudeKm );
    EXPECT_FLOAT_EQ( shipped.EdgeTopFraction, builtIn.EdgeTopFraction );
    EXPECT_FLOAT_EQ( shipped.BaseRampFraction, builtIn.BaseRampFraction );
    for ( uint32_t i = 0; i < Desert::Graphic::kCloudProfileSamples; ++i )
        EXPECT_FLOAT_EQ( shipped.Profile.HalfWidth[i], builtIn.Profile.HalfWidth[i] ) << i;
    EXPECT_FLOAT_EQ( shipped.AnvilAltitudeKm, builtIn.AnvilAltitudeKm );
    EXPECT_FLOAT_EQ( shipped.AnvilThicknessKm, builtIn.AnvilThicknessKm );
    EXPECT_FLOAT_EQ( shipped.AnvilStrength, builtIn.AnvilStrength );
    EXPECT_FLOAT_EQ( shipped.DetailCharacter, builtIn.DetailCharacter );
    EXPECT_FLOAT_EQ( shipped.DetailFactor, builtIn.DetailFactor );
    EXPECT_FLOAT_EQ( shipped.DensityFactor, builtIn.DensityFactor );
    EXPECT_FLOAT_EQ( shipped.ExtinctionFactor, builtIn.ExtinctionFactor );
    EXPECT_FLOAT_EQ( shipped.PlacementScale, builtIn.PlacementScale );
    EXPECT_FLOAT_EQ( shipped.PlacementAnisotropy, builtIn.PlacementAnisotropy );
}

TEST( CloudTypeLibrary, TheFourTypesT0ShippedAreUnchanged )
{
    // The v4 -> v5 migration is a RENAME and not a reinterpretation, and this is what makes that claim
    // true: a scene that said "Species 3" before this task renders the same cumulonimbus after it. The
    // numbers are T0's own table (commit 68fcc34e), copied here as literals on purpose — comparing the
    // library against itself would assert nothing.
    struct Expected
    {
        const char* Name;
        float       BaseKm;
        float       TopKm;
        float       Edge;
        float       Ramp;
        float       Taper;
        float       AnvilKm;
        float       AnvilThickness;
        float       AnvilStrength;
        float       Detail;
        float       Density;
    };

    const Expected rows[] = {
         { kCloudTypeStratus, 0.15f, 0.55f, 0.88f, 0.12f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0.70f },
         { kCloudTypeCumulusMediocris, 0.90f, 1.90f, 0.45f, 0.06f, 0.45f, 0.0f, 0.0f, 0.0f, 0.70f, 1.00f },
         { kCloudTypeCumulusCongestus, 2.20f, 5.80f, 0.15f, 0.04f, 0.50f, 0.0f, 0.0f, 0.0f, 1.00f, 1.15f },
         { kCloudTypeCumulonimbus, 0.90f, 9.00f, 0.12f, 0.04f, 0.40f, 9.5f, 1.8f, 0.85f, 0.85f, 1.35f },
    };

    for ( const Expected& row : rows )
    {
        const CloudTypeShape shape = LoadShipped( row.Name ).Shape;

        EXPECT_FLOAT_EQ( shape.BaseAltitudeKm, row.BaseKm ) << row.Name;
        EXPECT_FLOAT_EQ( shape.TopAltitudeKm, row.TopKm ) << row.Name;
        EXPECT_FLOAT_EQ( shape.EdgeTopFraction, row.Edge ) << row.Name;
        EXPECT_FLOAT_EQ( shape.BaseRampFraction, row.Ramp ) << row.Name;
        // THE TAPER SURVIVES AS THE CURVE IT GENERATES. Format version 3 replaced the number with the
        // sampled law; asserting the SAMPLES against `CloudProfileFromTaper(row.Taper)` is the same claim
        // this line always made — that the file still says what T0's table said — expressed in the form
        // the file now says it in. If this ever fails, the library was re-authored rather than
        // re-expressed, which is the thing the whole test exists to forbid.
        const Desert::Graphic::CloudVerticalProfile expected = Desert::Graphic::CloudProfileFromTaper( row.Taper );
        for ( uint32_t i = 0; i < Desert::Graphic::kCloudProfileSamples; ++i )
            EXPECT_FLOAT_EQ( shape.Profile.HalfWidth[i], expected.HalfWidth[i] ) << row.Name << " sample " << i;
        EXPECT_FLOAT_EQ( shape.AnvilAltitudeKm, row.AnvilKm ) << row.Name;
        EXPECT_FLOAT_EQ( shape.AnvilThicknessKm, row.AnvilThickness ) << row.Name;
        EXPECT_FLOAT_EQ( shape.AnvilStrength, row.AnvilStrength ) << row.Name;
        EXPECT_FLOAT_EQ( shape.DetailCharacter, row.Detail ) << row.Name;
        EXPECT_FLOAT_EQ( shape.DensityFactor, row.Density ) << row.Name;
    }
}

TEST( CloudTypeLibrary, EveryTypeSitsWhereMeteorologyPutsIt )
{
    // THE ABSOLUTE ANCHOR, and it is on the CONTENT now rather than on a table in a header, because the
    // content is where the numbers live. A set of ratios is satisfied at any scale; these are metres above
    // the ground, and a library that drifts out of them is a library that has stopped describing weather.
    //
    // The bands are the standard ones (WMO cloud classification, and Nubis Cubed's own p.11 catalogue).

    // Stratus: a sheet on the ground. Never above 600 m.
    EXPECT_LE( ProfileTopKm( LoadShipped( kCloudTypeStratus ).Shape ), 0.6f );
    EXPECT_GE( ProfileBaseKm( LoadShipped( kCloudTypeStratus ).Shape ), 0.0f );

    // Cumulus mediocris: base on the condensation level, top a kilometre up. 0.8 to 2.0 km.
    EXPECT_GE( ProfileBaseKm( LoadShipped( kCloudTypeCumulusMediocris ).Shape ), 0.8f );
    EXPECT_LE( ProfileTopKm( LoadShipped( kCloudTypeCumulusMediocris ).Shape ), 2.0f );

    // Cumulus humilis: the same base, and it STOPS. Flatter than mediocris is the whole species.
    EXPECT_GE( ProfileBaseKm( LoadShipped( kCloudTypeCumulusHumilis ).Shape ), 0.8f );
    EXPECT_LE( ProfileTopKm( LoadShipped( kCloudTypeCumulusHumilis ).Shape ), 1.5f );

    // Cumulonimbus: base in the 0.5-1.5 km band, top past eight kilometres.
    EXPECT_GE( ProfileBaseKm( LoadShipped( kCloudTypeCumulonimbus ).Shape ), 0.5f );
    EXPECT_LE( ProfileBaseKm( LoadShipped( kCloudTypeCumulonimbus ).Shape ), 1.5f );
    EXPECT_GT( ProfileTopKm( LoadShipped( kCloudTypeCumulonimbus ).Shape ), 8.0f );

    // Stratocumulus: a LOW deck. Base below a kilometre, top below two and a half.
    EXPECT_LE( ProfileBaseKm( LoadShipped( kCloudTypeStratocumulus ).Shape ), 1.0f );
    EXPECT_LE( ProfileTopKm( LoadShipped( kCloudTypeStratocumulus ).Shape ), 2.5f );

    // Altocumulus: the MID level, 2 to 7 km, and thin with it.
    const CloudTypeShape alto = LoadShipped( kCloudTypeAltocumulus ).Shape;
    EXPECT_GE( ProfileBaseKm( alto ), 2.0f );
    EXPECT_LE( ProfileTopKm( alto ), 7.0f );
    EXPECT_LE( ProfileTopKm( alto ) - ProfileBaseKm( alto ), 1.5f ) << "an altocumulus a kilometre and a "
                                                                       "half thick is a stratocumulus that "
                                                                       "has moved house";

    // Cirrus: ICE, and ice does not form below six kilometres.
    EXPECT_GE( ProfileBaseKm( LoadShipped( kCloudTypeCirrus ).Shape ), 6.0f );

    // Lenticular: it stands over a mountain, in the low-to-mid band.
    EXPECT_GE( ProfileBaseKm( LoadShipped( kCloudTypeLenticular ).Shape ), 1.5f );
    EXPECT_LE( ProfileTopKm( LoadShipped( kCloudTypeLenticular ).Shape ), 6.0f );
}

TEST( CloudTypeLibrary, ThePlacementFieldSaysWhatKindOfPatchEachTypeMakes )
{
    // THE THREE DEFECTS T3 WAS HANDED BY NAME, pinned to the numbers that answer them. Each of the three
    // was named as unreachable by any profile — a profile decides a silhouette in ELEVATION, and all three
    // are about the shape of a patch in PLAN.
    //
    // 1. CIRRUS READS AS A MACKEREL SKY RATHER THAN AS FIBROUS BANDS. A band is a patch far longer along
    //    the wind than across it, and until a type carried its own anisotropy the placement field was one
    //    field, isotropic, shared by everything in the sky.
    const CloudTypeShape cirrus = LoadShipped( kCloudTypeCirrus ).Shape;
    EXPECT_GE( cirrus.PlacementAnisotropy, 4.0f )
         << "cirrus is not stretched along the wind, so it is a field of thin blobs rather than fibres";

    // 2. LENTICULAR AND ALTOCUMULUS DIFFER QUANTITATIVELY RATHER THAN IN KIND. They now differ in the SIGN
    //    of the stretch: a wave cloud's crest lies ACROSS the flow (anisotropy below 1), a mackerel sky's
    //    rows lie ALONG it (above 1). That is a difference of kind and not of degree, and no setting of
    //    the two profiles could have produced it.
    const CloudTypeShape lenticular = LoadShipped( kCloudTypeLenticular ).Shape;
    const CloudTypeShape alto       = LoadShipped( kCloudTypeAltocumulus ).Shape;

    EXPECT_LT( lenticular.PlacementAnisotropy, 1.0f )
         << "the lenticular is not stretched across the wind, so it is not a wave cloud";
    EXPECT_GT( alto.PlacementAnisotropy, 1.0f ) << "the altocumulus is not rowed along the wind";

    // 3. STRATOCUMULUS IN THE ZENITH: ONE CELL FILLING THE FRAME. The layer's shipped Weather Tile Size is
    //    12 km and its coarse cell is a quarter of that — 3 km — under a deck 1 km thick. A cell three
    //    times the layer's own depth is one lump from horizon to horizon looking up. The type's own scale
    //    is what fixes it, and the number below is the one that puts the cell near the deck's thickness.
    const CloudTypeShape strato = LoadShipped( kCloudTypeStratocumulus ).Shape;

    constexpr float kLayerTileKm = 12.0f;
    constexpr float kCellOfTile  = 0.25f;

    const float cellKm  = kLayerTileKm * kCellOfTile * strato.PlacementScale;
    const float depthKm = ProfileTopKm( strato ) - ProfileBaseKm( strato );

    EXPECT_LT( cellKm, 2.0f * depthKm )
         << "a stratocumulus cell of " << cellKm << " km under a deck " << depthKm
         << " km thick is one lump filling the zenith, which is the defect this number answers";

    // AND THE LIBRARY DOES NOT COLLAPSE ONTO ONE PLACEMENT. Nine types that all placed themselves at the
    // layer's tile would be the T3 version of "nine labels on one cloud": the profiles would differ and
    // every patch would be the same size and shape.
    const char* names[] = { kCloudTypeStratus,          kCloudTypeCumulusHumilis, kCloudTypeCumulusMediocris,
                            kCloudTypeCumulusCongestus, kCloudTypeCumulonimbus,   kCloudTypeStratocumulus,
                            kCloudTypeAltocumulus,      kCloudTypeCirrus,         kCloudTypeLenticular };

    float smallest = 1e9f;
    float largest  = 0.0f;
    for ( const char* name : names )
    {
        const CloudTypeShape shape = LoadShipped( name ).Shape;
        smallest                   = std::min( smallest, shape.PlacementScale );
        largest                    = std::max( largest, shape.PlacementScale );
    }

    std::printf( "[CloudTypeLibrary] shipped placement scales span %.2f to %.2f of the layer's tile\n", smallest,
                 largest );

    EXPECT_GT( largest / smallest, 4.0f )
         << "every shipped type places itself at nearly the same scale, so the per-type placement field is "
            "authored but says nothing";
}

TEST( CloudTypeLibrary, NoTwoTypesAreTheSameCloudUnderTwoNames )
{
    // THE PLANK IS DISTINGUISHABILITY, NOT COUNT. A type that renders as its neighbour is a signature, not
    // a kind of cloud, and shipping nine of those would be the "nine labels on one cloud" failure
    // PLAN_CLOUD_TYPES.md §1 warns about — in the exact place it warned it would appear.
    //
    // Two types are held to be distinguishable when they differ by a fifth in the ALTITUDE BAND they
    // occupy, or by a third in how much opaque matter a column of them holds. Those are the two things a
    // camera on the ground can see: where the cloud is, and how solidly it blocks the sky.
    const char* names[] = {
         kCloudTypeStratus,          kCloudTypeCumulusHumilis, kCloudTypeCumulusMediocris,
         kCloudTypeCumulusCongestus, kCloudTypeCumulonimbus,   kCloudTypeStratocumulus,
         kCloudTypeAltocumulus,      kCloudTypeCirrus,         kCloudTypeLenticular,
    };

    std::vector<CloudTypeShape> shapes;
    for ( const char* name : names )
        shapes.push_back( LoadShipped( name ).Shape );

    for ( size_t a = 0; a < shapes.size(); ++a )
    {
        for ( size_t b = a + 1; b < shapes.size(); ++b )
        {
            const float baseGap = std::fabs( CloudTypeBaseKm( shapes[a] ) - CloudTypeBaseKm( shapes[b] ) );
            const float topGap  = std::fabs( CloudTypeTopKm( shapes[a] ) - CloudTypeTopKm( shapes[b] ) );
            const float span    = std::max( CloudTypeTopKm( shapes[a] ) - CloudTypeBaseKm( shapes[a] ),
                                            CloudTypeTopKm( shapes[b] ) - CloudTypeBaseKm( shapes[b] ) );

            const float opacityA = ColumnOpacity( shapes[a] );
            const float opacityB = ColumnOpacity( shapes[b] );
            const float opacityRatio =
                 std::max( opacityA, opacityB ) / std::max( std::min( opacityA, opacityB ), 1e-6f );

            const bool differentPlace  = ( baseGap + topGap ) > 0.2f * span;
            const bool differentMatter = opacityRatio > 1.33f;

            EXPECT_TRUE( differentPlace || differentMatter )
                 << names[a] << " and " << names[b] << " occupy the same band (bases "
                 << CloudTypeBaseKm( shapes[a] ) << " / " << CloudTypeBaseKm( shapes[b] ) << " km, tops "
                 << CloudTypeTopKm( shapes[a] ) << " / " << CloudTypeTopKm( shapes[b] )
                 << " km) and hold the same matter (" << opacityA << " / " << opacityB
                 << ") — one of them is a signature rather than a kind of cloud";
        }
    }
}

TEST( CloudTypeLibrary, OnlyTheTypesThatNeedTheirOwnNoiseNameOne )
{
    // A type's noise reference is what makes "the edge belongs to the kind of cloud" true rather than
    // stated. Cirrus is the one that needs it — ice at eight kilometres is wisps, not lobes — and it names
    // the FINER of the two shipped volumes. Every other type leaves the field absent, which is the
    // documented "use the built-in default", and a library where all nine named a volume would be a
    // library that had turned a meaningful choice into boilerplate.
    const CloudTypeData cirrus = LoadShipped( kCloudTypeCirrus );
    if ( !cirrus.NoiseVolume || !cirrus.Header )
    {
        ADD_FAILURE() << "the one type whose edge is its identity names no volume (or states no header)";
        return;
    }
    EXPECT_EQ( cirrus.NoiseVolume->Path, "Clouds/CloudNoise_FineWisp.dcnv" );
    // BY GUID since CLTY 5 (T7h): the volume's envelope GUID resolves it, and the header states it again as
    // its one Dependency - the registry's edge and the resolver's volume are one reference.
    EXPECT_EQ( cirrus.Header->Dependencies, std::vector<std::string>{ cirrus.NoiseVolume->Guid } );
    const auto guid = Common::Content::AssetGuidFromText( cirrus.NoiseVolume->Guid );
    EXPECT_TRUE( guid && !guid.GetValue().IsNull() ) << "'" << cirrus.NoiseVolume->Guid << "'";

    for ( const char* name :
          { kCloudTypeStratus, kCloudTypeCumulusMediocris, kCloudTypeCumulusCongestus, kCloudTypeCumulonimbus,
            kCloudTypeStratocumulus, kCloudTypeAltocumulus, kCloudTypeCumulusHumilis, kCloudTypeLenticular } )
    {
        EXPECT_FALSE( LoadShipped( name ).NoiseVolume.has_value() )
             << name << " names a noise volume; only the type whose edge is its identity should";
    }
}

TEST( CloudTypeLibrary, EveryShippedTypeIsOneTheLoaderWouldAccept )
{
    // The shipped library and the loader's own rules, held against each other. A preset that Validate
    // rejects would be a file the engine refuses to open — shipped, in the repository, and only ever
    // discovered by whoever selected it in the dropdown.
    for ( const char* name : { kCloudTypeStratus, kCloudTypeCumulusHumilis, kCloudTypeCumulusMediocris,
                               kCloudTypeCumulusCongestus, kCloudTypeCumulonimbus, kCloudTypeStratocumulus,
                               kCloudTypeAltocumulus, kCloudTypeCirrus, kCloudTypeLenticular } )
    {
        const CloudTypeData data = LoadShipped( name );
        EXPECT_TRUE( ValidateCloudTypeShape( data.Shape ) ) << name;
        EXPECT_TRUE( data.DisplayName.has_value() ) << name << " has no display name, so a slot would show "
                                                    << "its file stem instead of what it is";
        EXPECT_TRUE( data.Notes.has_value() ) << name << " carries no note saying what weather it is for";
    }
}

// ---------------------------------------------------------------------------------------------------
// THE LIBRARY AGAINST THE MARCH: no shipped type may place structure the march cannot find
// ---------------------------------------------------------------------------------------------------

namespace
{
    // The component's Max Steps default. It is a QUALITY TIER — an artist who lowers it is buying speed
    // with resolution on purpose — so the library is calibrated against the default rather than against
    // whatever a scene happens to carry.
    constexpr float kComponentMaxSteps = 256.0f;
} // namespace

// ---------------------------------------------------------------------------------------------------
// THE RELATION THIS PROGRAMME HAS BEEN BITTEN BY TWICE: what the library PLACES against what the march
// can FIND
// ---------------------------------------------------------------------------------------------------
//
// HOW THIS TEST CHANGED IN PHASE Э5, and it is worth stating because the shape of the answer changed and
// not only the arithmetic.
//
// It used to derive the finest CELL of a type's placement noise — three frequencies from
// CloudSpeciesPlacementBasis against the lattice of the `.dcnv` it names — multiply by a measured
// chord-per-cell of 0.5, and assert the result against the march's search step. It was a good test and it
// found five of nine types past Nyquist. It measured a chain that no longer exists: there is no placement
// noise, and a type's structure is the LUMPS the generator places.
//
// So it measures the lumps, and the relation is the same relation, held closer to the thing it is about:
// the smallest semi-axis of any lump the type places, doubled, against the chord the march resolves.
// Closer, because the old version needed a measured 0.5 to get from a cell to a chord and this needs
// nothing — a lump's diameter IS a chord through it.
//
// AND THE ANSWER IS NOW A CLAMP RATHER THAN A HOPE. The generator floors every semi-axis at half the
// chord it is handed, so this cannot fail for a shipped type unless the clamp is removed. That is the
// point: it is the line that fails the day somebody decides the clamp is unnecessary.
TEST( CloudTypeLibrary, NoShippedTypePlacesStructureThinnerThanTheMarchCanFind )
{
    using namespace Desert::Tests::CloudScheduleRef;

    const float resolvableKm = CloudFinestResolvableChordKm( kComponentMaxSteps );
    ASSERT_GT( resolvableKm, 0.0f );

    std::printf( "[CloudTypeLibrary] the march resolves chords down to %.0f m (fine step %.1f m, search "
                 "step %.1f m at Max Steps %.0f)\n",
                 resolvableKm * 1000.0f, CLOUD_DISTANCE_TO_MAX_STEPS_KM / kComponentMaxSteps * 1000.0f,
                 CLOUD_COARSE_STEP_MULTIPLIER * CLOUD_DISTANCE_TO_MAX_STEPS_KM / kComponentMaxSteps * 1000.0f,
                 kComponentMaxSteps );

    for ( const char* name : { kCloudTypeStratus, kCloudTypeCumulusHumilis, kCloudTypeCumulusMediocris,
                               kCloudTypeCumulusCongestus, kCloudTypeCumulonimbus, kCloudTypeStratocumulus,
                               kCloudTypeAltocumulus, kCloudTypeCirrus, kCloudTypeLenticular } )
    {
        const CloudTypeData data   = LoadShipped( name );
        const TypeColumn&   column = ColumnOf( data.Shape );

        ASSERT_FALSE( column.Blobs.empty() ) << name << " places nothing at full coverage";

        float smallestKm = column.Blobs.front().RadiiKm.x;
        for ( const Desert::Assets::CloudModellingBlob& blob : column.Blobs )
            smallestKm = std::min( { smallestKm, blob.RadiiKm.x, blob.RadiiKm.y, blob.RadiiKm.z } );

        const float chordKm = 2.0f * smallestKm;

        std::printf( "[CloudTypeLibrary] %-18s %5zu lumps, thinnest chord %6.0f m, %.2fx the %.0f m the "
                     "march resolves\n",
                     name, column.Blobs.size(), chordKm * 1000.0f, chordKm / resolvableKm,
                     resolvableKm * 1000.0f );

        EXPECT_GE( chordKm, resolvableKm )
             << name << " places a lump only " << chordKm * 1000.0f << " m across, thinner than the "
             << resolvableKm * 1000.0f
             << " m the march can be relied on to find, so that part of this type is sampled or not "
                "sampled by the throw of a jitter and reads as dither. The generator's own clamp is what "
                "makes this impossible — if this line fails, the clamp is gone";
    }
}

// ---------------------------------------------------------------------------------------------------
// THE SAME RELATION, READ BACKWARDS: how far Max Steps may fall before the library stops working
// ---------------------------------------------------------------------------------------------------
//
// THE ANSWER CHANGED IN PHASE Э5 AND THE TEST WITH IT. Before, a type's structure was fixed by its
// placement noise and the march's step was the only moving part, so a lower Max Steps put types past
// Nyquist — measured at five of nine when halved — and that measurement is why Graphic::CloudQualityScale
// has no Max Steps field at all.
//
// The generator is handed the march's own resolvable chord and floors every lump against it, so lowering
// Max Steps now buys COARSER CLOUDS rather than speckle. The bound is gone, and this asserts what
// replaced it: at every count a tier could plausibly march with, every shipped type still clears THAT
// count's chord.
//
// WHAT THIS MEANS FOR THE REFUSAL RECORDED IN CloudQualityScale: its carrying input has changed, so the
// refusal should be re-measured rather than trusted. That is a finding of this phase and it is stated
// here rather than acted on — a tier that lowers Max Steps is a cost-versus-quality decision with its own
// frames to shoot, and this phase did not shoot them.
TEST( CloudTypeLibrary, LoweringMaxStepsBuysCoarserCloudsRatherThanSpeckle )
{
    using namespace Desert::Tests::CloudScheduleRef;

    for ( const float maxSteps : { 256.0f, 192.0f, 128.0f, 64.0f } )
    {
        const float chordKm = CloudFinestResolvableChordKm( maxSteps );

        for ( const char* name : { kCloudTypeStratus, kCloudTypeCumulusHumilis, kCloudTypeCumulusMediocris,
                                   kCloudTypeCumulusCongestus, kCloudTypeCumulonimbus, kCloudTypeStratocumulus,
                                   kCloudTypeAltocumulus, kCloudTypeCirrus, kCloudTypeLenticular } )
        {
            const CloudTypeData data = LoadShipped( name );

            Desert::Assets::CloudProceduralFieldParams params = TypeParams( data.Shape );
            params.ResolvableChordKm                          = chordKm;

            const glm::vec2 origin = Desert::Assets::CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

            const std::vector<Desert::Assets::CloudModellingBlob> blobs =
                 Desert::Assets::GenerateCloudProceduralBlobs( params, 0u, origin );

            ASSERT_FALSE( blobs.empty() ) << name << " at Max Steps " << maxSteps << " placed nothing";

            for ( const Desert::Assets::CloudModellingBlob& blob : blobs )
            {
                const float thinnestKm = 2.0f * std::min( { blob.RadiiKm.x, blob.RadiiKm.y, blob.RadiiKm.z } );

                EXPECT_GE( thinnestKm, chordKm )
                     << name << " at Max Steps " << maxSteps << " places a lump " << thinnestKm * 1000.0f
                     << " m across against a search chord of " << chordKm * 1000.0f
                     << " m — the generator is not reading the count it is handed";
            }
        }

        std::printf( "[CloudTypeLibrary] at Max Steps %3.0f the march resolves %5.0f m and every shipped "
                     "type clears it\n",
                     maxSteps, chordKm * 1000.0f );
    }
}

// ---------------------------------------------------------------------------------------------------
// THE LIBRARY AGAINST THE LAYER'S EROSION SLIDER — task DS, 2026-08-24
// ---------------------------------------------------------------------------------------------------

TEST( CloudTypeLibrary, TheLayersDetailStrengthStillMovesEveryShippedType )
{
    // THE RELATION. The depth of the erosion's cut is `clamp(DetailStrength * DetailFactor, 0, 1)` — the
    // layer's slider times the TYPE's own multiplier, formed per sample in Common/CloudField.glslh because
    // one product formed on the CPU could only describe one kind of cloud. That clamp is not decoration:
    // the erosion is subtracted from a profile that lives in [0, 1] and a depth above 1 would collapse the
    // remap's window.
    //
    // WHAT IT COSTS WHEN IT CLOSES. A type whose product has reached 1 stops responding to the layer's
    // slider ALTOGETHER: the artist drags Detail Strength and that type does not change. This programme
    // calls a control that moves nothing a TODO wearing a feature's clothes (contract §1.3), and here it
    // would be a control that moves eight types out of nine.
    //
    // WHY IT IS ASSERTED HERE AND NOT ON THE COMPONENT. The bound is a property of the LIBRARY — of the
    // largest DetailFactor anyone shipped — and the library is nine files on disk. This suite is the one
    // that opens them. The component's own default is read rather than transcribed, so the two sides of
    // the relation are the two real ones.
    //
    // AND THERE IS A SECOND, TIGHTER BOUND THAN THE CLAMP, MEASURED ON THE FRAME. A type does not have to
    // reach the clamp to be ruined by the erosion; a THIN type dissolves well before it. Task DS raised
    // the layer's Detail Strength from 0.10 to 0.40 — for the march's sake, see the component — and shot
    // every type against a CLOUDLESS frame at the same camera to measure what survived:
    //
    //     type          factor   effective cut   its contribution to the frame, as a share of un-eroded
    //     cirrus         2.50    0.25 -> 1.00    33.3 %  ->  4.3 %
    //     altocumulus    1.60    0.16 -> 0.64    51.5 %  ->  7.5 %
    //
    // Both were authored against a layer of 0.10 and both are half the density of a cumulus, so the same
    // arithmetic that gives a congestus an edge deletes them. Their factors were re-based — 0.625 and
    // 0.40 — which restores their effective cut DEPTH to what their files were authored at, exactly, and
    // restores nothing else: Detail Tile Size is the LAYER's and it moved for all nine types, so both now
    // meet an erosion four times finer than their files ever saw. The amount of each type in the sky is
    // preserved to about one per cent; where its material sits is not (CALIBRATION.md §DS).
    //
    // So the bound asserted below is the reference type's own cut: no type may be cut DEEPER than the
    // congestus whose factor is 1 by definition, because past that depth the measurement says a thin body
    // stops being a cloud. A type that genuinely wants a deeper cut needs the density to carry it, and
    // that is content work with its own frames.
    // The layer's Detail Strength is the MATERIAL's since O1, and CloudMaterialValues is the mirror of
    // the CloudRaymarch schema the CloudMaterialSchema suite pins. Read rather than transcribed: this
    // whole test is about the product of the layer's value and the type's factor, so a copy of one
    // side would let the bound go on passing after the value it bounds had moved.
    const Desert::Graphic::CloudMaterialValues layer;

    float       largestFactor = 0.0f;
    const char* largestName   = "";

    for ( const char* name : { kCloudTypeStratus, kCloudTypeCumulusHumilis, kCloudTypeCumulusMediocris,
                               kCloudTypeCumulusCongestus, kCloudTypeCumulonimbus, kCloudTypeStratocumulus,
                               kCloudTypeAltocumulus, kCloudTypeCirrus, kCloudTypeLenticular } )
    {
        const CloudTypeData data = LoadShipped( name );

        const float depth = layer.DetailStrength * data.Shape.DetailFactor;

        std::printf( "[CloudTypeLibrary] %-18s detail factor %.2f -> cut depth %.3f at the layer's %.2f\n", name,
                     data.Shape.DetailFactor, depth, layer.DetailStrength );

        if ( data.Shape.DetailFactor > largestFactor )
        {
            largestFactor = data.Shape.DetailFactor;
            largestName   = name;
        }

        EXPECT_LE( depth, 1.0f )
             << name << " has a detail factor of " << data.Shape.DetailFactor
             << ", which at the layer's shipped Detail Strength of " << layer.DetailStrength
             << " gives a cut depth of " << depth
             << " — past the clamp in Common/CloudField.glslh, so the layer's slider moves this type by "
                "nothing at all from here upward";

        // AND THE OTHER END: a factor of zero is a type that ignores the slider in the other direction.
        // The lenticular's 0.15 is the smallest shipped and is deliberate — a lens has to stay a lens —
        // but zero would be a type nobody can erode, which is what Detail Factor 0 is FOR and is
        // therefore allowed. What is asserted is only that the shipped library does not do it by accident.
        EXPECT_GT( data.Shape.DetailFactor, 0.0f )
             << name << " cannot be eroded at all, which is a smooth silhouette however the layer is set";

        // THE MEASURED BOUND. The reference type's factor is 1 by definition, so this says "no shipped
        // type is cut deeper than the reference". At 1.6 and 2.5 the frames measured 7.5 % and 4.3 % of
        // the type left in the picture — a type that is gone is not a wispy type.
        EXPECT_LE( data.Shape.DetailFactor, 1.0f )
             << name << " is cut " << data.Shape.DetailFactor
             << " times as deep as the reference congestus. Measured on the frame, a factor above 1 at the "
                "shipped Detail Strength dissolves a thin type rather than shredding it: the cirrus at 2.5 "
                "kept 4.3 % of its contribution to the picture and the altocumulus at 1.6 kept 7.5 %";
    }

    std::printf( "[CloudTypeLibrary] the largest factor shipped is %s at %.2f, which fixes the layer's "
                 "Detail Strength ceiling at %.3f\n",
                 largestName, largestFactor, 1.0f / largestFactor );

    // Stated the other way round, so the failure names the ceiling rather than the type: the shipped
    // Detail Strength may not exceed the reciprocal of the largest factor anyone authored.
    EXPECT_LE( layer.DetailStrength, 1.0f / largestFactor )
         << "the layer's Detail Strength of " << layer.DetailStrength << " is above the " << 1.0f / largestFactor
         << " that the library's largest Detail Factor (" << largestName << " at " << largestFactor
         << ") leaves room for";
}

// THE RE-BASE IS AN IDENTITY, AND AN IDENTITY IS A THING A TEST CAN HOLD.
//
// WHAT A RE-BASE IS FOR. The depth of a type's cut is `DetailStrength * DetailFactor` — one number owned by
// the LAYER times one owned by the TYPE. When the layer's number has to move for a reason that has nothing
// to do with any particular type, every type's cut moves with it, and for a THIN type a deeper cut does not
// shred it, it deletes it: task DS measured the cirrus keeping 4.3 % of its contribution to the frame and
// the altocumulus 7.5 %. The repair is to divide the type's factor by the same ratio the layer's number was
// multiplied by, so that the product — the only quantity the shader ever forms — does not move at all.
//
// IT HAS NOW BEEN DONE TWICE, BY TWO TASKS, FOR TWO UNRELATED REASONS:
//
//     authored      strength 0.10 x factor 2.50 = 0.250   cirrus
//     §DS           strength 0.40 x factor 0.625 = 0.250   the march needed a deeper layer
//     §SIL2         strength 0.65 x factor 0.3846154 = 0.250   a taller lump needed a deeper layer again
//
//     authored      strength 0.10 x factor 1.60 = 0.160   altocumulus
//     §DS           strength 0.40 x factor 0.40 = 0.160
//     §SIL2         strength 0.65 x factor 0.2461538 = 0.160
//
// WHY IT IS ASSERTED RATHER THAN RECOMPUTED IN A COMMENT. Both re-bases were arithmetic done by hand in a
// report, and the numbers then had to be typed into a JSON file by hand as well. A slip in either — a digit,
// or a third task raising the layer and re-basing only one of the two files — is invisible: nothing renders
// an error, the type simply gets thinner, and the next person measures a library that has drifted from its
// own authored intent with no record of when. The depth is the invariant; this is the line that holds it.
//
// AND THE TOLERANCE IS THE FILE'S, NOT THE MATHS'. 0.25 / 0.65 is not representable in decimal, so the asset
// carries seven digits of it and the product lands within about one part in ten million of 0.250. The bound
// below is a thousand times looser than that and a thousand times tighter than a typo.
TEST( CloudTypeLibrary, TheReBasedTypesKeepTheCutDepthTheirFilesWereAuthoredAt )
{
    // The layer's Detail Strength is the MATERIAL's since O1, and CloudMaterialValues is the mirror of
    // the CloudRaymarch schema the CloudMaterialSchema suite pins. Read rather than transcribed: this
    // whole test is about the product of the layer's value and the type's factor, so a copy of one
    // side would let the bound go on passing after the value it bounds had moved.
    const Desert::Graphic::CloudMaterialValues layer;

    // The depth each of these files has been authored at since before task DS, and which no re-base of the
    // layer's Detail Strength is permitted to move.
    struct AuthoredDepth
    {
        const char* Name;
        float       Depth;
    };

    for ( const AuthoredDepth& authored :
          { AuthoredDepth{ kCloudTypeCirrus, 0.25f }, AuthoredDepth{ kCloudTypeAltocumulus, 0.16f } } )
    {
        const CloudTypeData data = LoadShipped( authored.Name );

        const float depth = layer.DetailStrength * data.Shape.DetailFactor;

        std::printf( "[CloudTypeLibrary] %-18s %.7f x the layer's %.2f = %.6f, authored at %.3f\n", authored.Name,
                     data.Shape.DetailFactor, layer.DetailStrength, depth, authored.Depth );

        EXPECT_NEAR( depth, authored.Depth, 1e-4f )
             << authored.Name << " is cut to a depth of " << depth << " at the layer's shipped Detail Strength of "
             << layer.DetailStrength << ", but its file was authored for a cut of " << authored.Depth
             << ".\nThis type's Detail Factor is a RE-BASE and not an art direction: whenever the layer's "
                "Detail Strength moves, this factor is divided by the same ratio so that the product does "
                "not. If the layer was just raised, this file was not re-based with it — and a thin type "
                "meeting a deeper cut is not a wispier type, it is a missing one (CALIBRATION.md §DS "
                "measured 4.3 % of the cirrus surviving). If this factor was edited on purpose, the depth "
                "above is the number that was actually changed, and it is a re-art-direction that needs its "
                "own frames.";
    }
}

// ===================================================================================================
// О11 — THE SCENE'S OWN LIFT OF ITS DECK
//
// A type's altitudes belong to the TYPE, so raising them raises that cloud in every scene that loads the
// file. The owner asked for one scene's deck to sit higher, which is a thing this engine could not say at
// all: the shell was the union of the types' bands and nothing else could reach it.
//
// What was added is a DISPLACEMENT, not a second altitude, and the difference is the whole of the design.
// An absolute Layer Bottom on the component — which is what Unreal exposes, because Unreal's cloud types
// carry no altitudes — would be a second author for a value the types already own, and the symptom of the
// two disagreeing is not an error but a tower sliced off by a ceiling nobody remembers setting. A
// displacement cannot disagree with an altitude: it is a different quantity, and the two compose.
//
// These tests assert the composition, and one of them asserts the NAIVE version is wrong, because that is
// the trap the design exists to avoid and a comment saying so is not the code that says so.
// ===================================================================================================

namespace
{
    // The nine shipped files. Spelled out again here because this suite has never had one statement of
    // the list — the same nine names are written out seven times above, which is a census that can drift
    // from itself and is worth one task of its own. These tests use one list rather than adding an
    // eighth spelling to the pile.
    constexpr const char* kShippedLibrary[] = {
         kCloudTypeStratus,          kCloudTypeCumulusHumilis, kCloudTypeCumulusMediocris,
         kCloudTypeCumulusCongestus, kCloudTypeCumulonimbus,   kCloudTypeStratocumulus,
         kCloudTypeAltocumulus,      kCloudTypeCirrus,         kCloudTypeLenticular,
    };

    /// A layer holding one shipped type, resolved exactly as the renderer's ResolveSpecies would leave it
    /// before the lift is applied.
    Desert::ECS::VolumetricCloudData LayerWithLift( float liftKm )
    {
        Desert::ECS::VolumetricCloudData data;
        data.LayerAltitudeOffset = liftKm;
        return data;
    }

    /// The bake's own shell and bodies for a set of shapes, through the function the renderer calls.
    Desert::Assets::CloudProceduralFieldParams BakeParamsFor( const CloudTypeShape* shapes, uint32_t count )
    {
        Desert::Assets::CloudProceduralFieldParams params;
        Desert::Graphic::ApplyCloudMaterialToBakeParams( Desert::Graphic::CloudMaterialValues{},
                                                         Desert::Graphic::CloudBakeLayerInputs{}, shapes, count,
                                                         params );
        return params;
    }

    /// The march's shell for the same set, through the packer.
    glm::vec4 MarchLayerFor( const Desert::ECS::VolumetricCloudData& data, const CloudTypeShape* shapes,
                             uint32_t count )
    {
        return Desert::Graphic::PackCloudParams( data, Desert::Graphic::CloudMaterialValues{}, shapes, count,
                                                 Desert::Graphic::AtmosphereEnv{}, glm::vec3( 0.0f ) )
             .Layer;
    }
} // namespace

// The default is the old behaviour to the BIT, which is the negative control the eighty-four scenes in
// this repository stand on: not one of them states this field, a missing key leaves it at its default,
// and the default has to be the number that changes nothing.
TEST( CloudLayerLift, DefaultIsZeroAndLeavesEverySetByteIdentical )
{
    EXPECT_FLOAT_EQ( Desert::ECS::VolumetricCloudData{}.LayerAltitudeOffset, 0.0f );

    for ( const char* name : kShippedLibrary )
    {
        const CloudTypeShape authored = LoadShipped( name ).Shape;

        CloudTypeShape lifted = authored;
        EXPECT_FLOAT_EQ( Desert::Graphic::CloudLiftSpeciesSet( LayerWithLift( 0.0f ), &lifted, 1u ), 0.0f );

        EXPECT_EQ( std::memcmp( &lifted, &authored, sizeof( CloudTypeShape ) ), 0 )
             << name << " moved at a lift of zero — every scene in the repository would have shifted.";
    }
}

// Every altitude in the shape moves by the same amount, so the lift is a TRANSLATION and not a shape
// edit. The thickness is the assertion that matters: a lift that moved the base without the top would
// stretch the band, the fixed voxel rows of the modelling volume would be spread over more kilometres,
// and a stratus deck would stop being expressible by the trilinear fetch — the exact cost the anvil
// predicate's comment measures for a shell that is not the shell the bake fills.
TEST( CloudLayerLift, MovesTheWholeBandAndChangesNoThickness )
{
    for ( const char* name : kShippedLibrary )
    {
        const CloudTypeShape authored = LoadShipped( name ).Shape;

        for ( const float liftKm : { 0.4f, 3.0f, 12.0f } )
        {
            CloudTypeShape lifted = authored;
            EXPECT_FLOAT_EQ( Desert::Graphic::CloudLiftSpeciesSet( LayerWithLift( liftKm ), &lifted, 1u ),
                             liftKm );

            EXPECT_FLOAT_EQ( lifted.BaseAltitudeKm, authored.BaseAltitudeKm + liftKm ) << name;
            EXPECT_FLOAT_EQ( lifted.TopAltitudeKm, authored.TopAltitudeKm + liftKm ) << name;
            EXPECT_FLOAT_EQ( lifted.AnvilAltitudeKm, authored.AnvilAltitudeKm + liftKm ) << name;

            // The band, the canopy's own slab and the anvil predicate all survive the move unchanged.
            //
            // NEAR AND NOT EQUAL, with the tolerance measured rather than guessed. A translation in
            // float is not exact: at the ceiling lift the lenticular's 0.800000 km band comes back as
            // 0.799999, because 2.6 + 12 and 3.4 + 12 round to neighbouring representables. 2e-5 km is
            // two centimetres against a band of eight hundred metres, and the modelling volume's own
            // vertical voxel at that band is 6.25 m — three hundred times coarser than the error.
            EXPECT_NEAR( lifted.TopAltitudeKm - lifted.BaseAltitudeKm,
                         authored.TopAltitudeKm - authored.BaseAltitudeKm, 2e-5f )
                 << name;
            EXPECT_FLOAT_EQ( lifted.AnvilThicknessKm, authored.AnvilThicknessKm ) << name;
            EXPECT_EQ( Desert::Graphic::CloudTypeHasAnvil( lifted ),
                       Desert::Graphic::CloudTypeHasAnvil( authored ) )
                 << name << " gained or lost its anvil by being moved, which is a shape edit and not a lift.";

            EXPECT_NEAR( Desert::Graphic::CloudTypeTopKm( lifted ),
                         Desert::Graphic::CloudTypeTopKm( authored ) + liftKm, 2e-5f )
                 << name;
        }
    }
}

// A `.desce` is a text file and an out-of-range number in one must produce a sky rather than a refusal —
// the same argument the four placement numbers make in ApplyCloudMaterialToBakeParams. The clamp is the
// component's own Range, read from the same constant the PROPERTY reads, so the two cannot part.
TEST( CloudLayerLift, OutOfRangeIsClampedToTheSliderRatherThanRefused )
{
    CloudTypeShape shape = LoadShipped( kCloudTypeLenticular ).Shape;

    CloudTypeShape tooHigh = shape;
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudLiftSpeciesSet( LayerWithLift( 900.0f ), &tooHigh, 1u ),
                     Desert::ECS::kCloudLayerAltitudeOffsetMaxKm );

    // Below the slider's floor is not a lowering, because the shell is floored at sea level in both the
    // packer and the bake while the bodies are not — a deck asked to sink would be sliced rather than
    // moved. The knob is one-directional and says so by refusing to go negative.
    CloudTypeShape belowFloor = shape;
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudLiftSpeciesSet( LayerWithLift( -5.0f ), &belowFloor, 1u ), 0.0f );
    EXPECT_EQ( std::memcmp( &belowFloor, &shape, sizeof( CloudTypeShape ) ), 0 );

    // NaN is a legal float in a text file and an illegal altitude. std::clamp on a NaN returns the low
    // bound here, which is the one answer that cannot put a cloud anywhere unexpected.
    CloudTypeShape notANumber = shape;
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudLiftSpeciesSet(
                          LayerWithLift( std::numeric_limits<float>::quiet_NaN() ), &notANumber, 1u ),
                     0.0f );
    EXPECT_EQ( std::memcmp( &notANumber, &shape, sizeof( CloudTypeShape ) ), 0 );
}

// THE RELATION, which is what this suite is for. The march's shell and the bake's shell are two
// independent computations of one quantity — CloudPayload.hpp and CloudMaterialBake.hpp each call
// CloudTypeSetEnvelopeKm for themselves — and a lift that reached one but not the other would put the
// volume on a different shell than the ray intersects. That is the "sky was a ceiling" defect, and the
// only reason it cannot happen here is that BOTH read the same lifted array.
TEST( CloudLayerLift, TheMarchsShellAndTheBakesShellRiseTogetherByExactlyTheLift )
{
    // Two species, deliberately: the envelope is a UNION, and a union of two bands that both moved has to
    // move without widening. One type would pass while a lift that scaled instead of translated was live.
    const CloudTypeShape pair[2] = { LoadShipped( kCloudTypeStratocumulus ).Shape,
                                     LoadShipped( kCloudTypeCumulusCongestus ).Shape };

    CloudTypeShape rest[2] = { pair[0], pair[1] };
    Desert::Graphic::CloudLiftSpeciesSet( LayerWithLift( 0.0f ), rest, 2u );

    const glm::vec4                                  restMarch = MarchLayerFor( LayerWithLift( 0.0f ), rest, 2u );
    const Desert::Assets::CloudProceduralFieldParams restBake  = BakeParamsFor( rest, 2u );

    // The shipped pair's envelope, quoted so a change to either file is visible here as a number.
    EXPECT_FLOAT_EQ( restMarch.y, 0.60f );
    EXPECT_FLOAT_EQ( restMarch.z, 5.20f ); // 5.80 - 0.60

    for ( const float liftKm : { 1.0f, 3.4f, 12.0f } )
    {
        CloudTypeShape lifted[2] = { pair[0], pair[1] };
        Desert::Graphic::CloudLiftSpeciesSet( LayerWithLift( liftKm ), lifted, 2u );

        const glm::vec4 march = MarchLayerFor( LayerWithLift( liftKm ), lifted, 2u );
        const Desert::Assets::CloudProceduralFieldParams bake = BakeParamsFor( lifted, 2u );

        EXPECT_FLOAT_EQ( march.y, restMarch.y + liftKm );
        EXPECT_NEAR( march.z, restMarch.z, 2e-5f ) << "the shell grew instead of rising";

        EXPECT_FLOAT_EQ( bake.LayerBottomKm, restBake.LayerBottomKm + liftKm );
        EXPECT_NEAR( bake.LayerThicknessKm, restBake.LayerThicknessKm, 2e-5f );

        // The two of them, against each other. This is the assertion a future lift applied in one file
        // and not the other would fail, and it is why the lift is applied upstream of both.
        EXPECT_FLOAT_EQ( march.y, bake.LayerBottomKm );
        EXPECT_FLOAT_EQ( march.z, bake.LayerThicknessKm );

        // AND THE BODIES WITH THEM. The bake places each lump at its species' own absolute altitude and
        // discards anything outside the shell, so "the shell rose" is only half the claim.
        ASSERT_EQ( bake.Species.size(), 2u );
        for ( const Desert::Assets::CloudProceduralSpecies& species : bake.Species )
        {
            EXPECT_GE( Desert::Graphic::CloudTypeBaseKm( species.Shape ), bake.LayerBottomKm );
            EXPECT_LE( Desert::Graphic::CloudTypeTopKm( species.Shape ),
                       bake.LayerBottomKm + bake.LayerThicknessKm );
        }
    }
}

// WHY THE SHAPES AND NOT THE SHELL, asserted rather than argued. Lifting `LayerBottomKm` alone is the
// obvious implementation and it empties the sky: the generator rejects every blob whose extent falls
// outside the shell, and after a shell-only lift EVERY body is below it. Written as a test because the
// next person to touch this will have the same obvious idea.
TEST( CloudLayerLift, LiftingTheShellAloneWouldPutEveryBodyOutsideIt )
{
    const CloudTypeShape shape = LoadShipped( kCloudTypeLenticular ).Shape;

    Desert::Assets::CloudProceduralFieldParams shellOnly = BakeParamsFor( &shape, 1u );
    const float                                topKm     = shellOnly.LayerBottomKm + shellOnly.LayerThicknessKm;

    constexpr float kLiftKm = 3.4f; // more than the lenticular's own 0.8 km band, so the miss is total
    shellOnly.LayerBottomKm += kLiftKm;

    // The species inside the params is the UNLIFTED one, which is exactly what a shell-only lift leaves.
    ASSERT_EQ( shellOnly.Species.size(), 1u );
    EXPECT_LE( Desert::Graphic::CloudTypeTopKm( shellOnly.Species[0].Shape ), shellOnly.LayerBottomKm )
         << "a shell-only lift no longer misses the bodies — if the generator's rejection rule changed, "
            "this design's reason changed with it.";

    EXPECT_LT( topKm, shellOnly.LayerBottomKm );
}

// A knob that reaches the parameters and not the CACHE is a knob that does nothing until something else
// happens to invalidate it, which is the least diagnosable kind of dead. The renderer's own rebake
// decision is asked here, unchanged.
TEST( CloudLayerLift, MovingTheKnobIsAReasonToRebake )
{
    const CloudTypeShape shape = LoadShipped( kCloudTypeLenticular ).Shape;

    CloudTypeShape rest = shape;
    Desert::Graphic::CloudLiftSpeciesSet( LayerWithLift( 0.0f ), &rest, 1u );

    CloudTypeShape lifted = shape;
    Desert::Graphic::CloudLiftSpeciesSet( LayerWithLift( 2.0f ), &lifted, 1u );

    EXPECT_FALSE(
         Desert::Assets::CloudProceduralParamsEqual( BakeParamsFor( &rest, 1u ), BakeParamsFor( &lifted, 1u ) ) );
}

// ---------------------------------------------------------------------------------------------------
// THE BAKE GRID AGAINST THE CELL THE TYPE AUTHORED — the relation, not either side of it
// ---------------------------------------------------------------------------------------------------
//
// Assets::CloudProceduralCellExtentKm floors a species' placement cell at four voxels, so the grid a view
// bakes on silently decides how far apart clusters are placed. Both sides look right on their own: the
// type authors a legitimate cell, the view asks for a legitimate grid, and the middle link enlarges one
// of them. At the shipped 48 km region the floor is 0.75 km at 256 voxels and 1.50 km at 128, and two of
// the nine shipped types author finer than the second — Altocumulus at 0.90 km, Stratocumulus at 1.05.
//
// The symptom is not a softer sky. Rendering SIL_Altocumulus at 128 against 256 from one camera moved
// 99.77 % of the frame, mean 35.1 of 255, against a repeat-shot floor of exactly 0 — the holes are in
// different places, because the lattice is. The asset preview bakes at 128, so an artist tuning either of
// those two types was authoring against clouds no level would draw.
//
// What is asserted is the RELATION: whatever grid a view asks for, the cell the bake ends up placing on
// is the cell the type authored. Graphic::CloudBakeSideForSpecies is what makes that true, by raising the
// grid; this fails the day the raise is removed, and it fails for a NEW type finer than 0.75 km, which is
// the case no frame has been shot for.
TEST( CloudTypeLibrary, NoBakeBudgetMovesTheCellAShippedTypeAuthored )
{
    for ( const int32_t askedSide : { 128, 160, 192, 256 } )
    {
        for ( const char* name : kShippedLibrary )
        {
            const CloudTypeShape shape = LoadShipped( name ).Shape;

            Desert::Graphic::CloudBakeLayerInputs layer;
            layer.VolumeResolution = askedSide;

            Desert::Assets::CloudProceduralFieldParams params;
            Desert::Graphic::ApplyCloudMaterialToBakeParams( Desert::Graphic::CloudMaterialValues{}, layer, &shape,
                                                             1u, params );

            ASSERT_EQ( params.Species.size(), 1u ) << name;

            const float authoredCellKm = params.Species.front().CellKm;
            const float voxelFloorKm = 4.0f * params.RegionSizeKm / static_cast<float>( params.VolumeSideVoxels );

            std::printf( "[CloudTypeLibrary] %-18s asked %3d voxels, baked %3u, cell %.2f km against a "
                         "%.2f km grid floor\n",
                         name, askedSide, params.VolumeSideVoxels, authoredCellKm, voxelFloorKm );

            EXPECT_LE( voxelFloorKm, authoredCellKm + 1e-4f )
                 << name << " authors a " << authoredCellKm << " km placement cell and the bake settled on a "
                 << params.VolumeSideVoxels << " grid, whose four-voxel floor is " << voxelFloorKm
                 << " km. The clusters would be laid out on a coarser lattice than the type asks for, which "
                    "is a different sky rather than a blurrier one";

            // AND IT ONLY EVER RAISES. A grid that fell below what the view asked for would be this
            // subsystem deciding a view's cost for it, which is the opposite failure and just as silent.
            EXPECT_GE( static_cast<int32_t>( params.VolumeSideVoxels ), askedSide ) << name;
        }
    }
}

// The negative control, and it is the half that makes the test above mean something: the seven types the
// cheap grid CAN carry must still bake on it, or the fix would simply be "always use 256" wearing a
// relation's clothes — and that would throw away O8's measured saving (229 ms against 961 ms) on every
// preview in the editor.
TEST( CloudTypeLibrary, TheCheapBakeGridIsKeptForEveryTypeThatFitsOnIt )
{
    int kept   = 0;
    int raised = 0;

    for ( const char* name : kShippedLibrary )
    {
        const CloudTypeShape shape = LoadShipped( name ).Shape;

        Desert::Graphic::CloudBakeLayerInputs layer;
        layer.VolumeResolution = static_cast<int32_t>( Desert::Assets::kCloudProceduralVolumeSideMin );

        Desert::Assets::CloudProceduralFieldParams params;
        Desert::Graphic::ApplyCloudMaterialToBakeParams( Desert::Graphic::CloudMaterialValues{}, layer, &shape, 1u,
                                                         params );

        const bool wasRaised = params.VolumeSideVoxels > Desert::Assets::kCloudProceduralVolumeSideMin;
        ( wasRaised ? raised : kept )++;

        // Named one by one rather than counted, because a count can be satisfied by a different two types
        // than the two that were measured.
        const bool expectRaise = ( std::string( name ) == kCloudTypeAltocumulus ) ||
                                 ( std::string( name ) == kCloudTypeStratocumulus );

        EXPECT_EQ( wasRaised, expectRaise )
             << name
             << ( expectRaise ? " authors a cell the cheap grid cannot carry and must raise it"
                              : " fits on the cheap grid and must keep it" );
    }

    std::printf( "[CloudTypeLibrary] cheap bake grid kept for %d of the shipped types, raised for %d\n", kept,
                 raised );
    EXPECT_EQ( kept, 7 );
    EXPECT_EQ( raised, 2 );
}

// WITNESS (AF7y, before T6c): A SHIPPED CLOUD MATERIAL'S TYPE SLOT NAMES A TYPE THE SERVICE KNOWS.
//
// Since format 4 (T6b1) a type registers in Runtime::CloudTypeService under HandleForGuid of its header
// GUID (CloudTypeAsset's constructor), and the slot is looked up by the bare number the `.demat` stores
// (CloudMaterialValues ApplyCloudAssetRef -> CloudTypeService::GetShape). This joins those two ends on the
// shipped files. EXPECTED RED UNTIL T6c: the `.demat` slots still carry the path-derived numbers minted
// before the header existed, and a miss renders the built-in default rather than failing anything - the
// AssetReferenceCensus stays green on it because its own handle rule hashes a `.decloudtype` by path.
// M_Clouds_Demo_Clouds rather than M_CloudDefault: the default material authors no slot at all.
TEST( CloudTypeLibrary, AShippedCloudMaterialsTypeSlotNamesARegisteredType )
{
    const std::filesystem::path types = LibraryDirectory();
    ASSERT_FALSE( types.empty() );
    const std::filesystem::path material =
         types.parent_path().parent_path() / "Materials" / "M_Clouds_Demo_Clouds.demat";

    std::ifstream in( material );
    ASSERT_TRUE( in.good() ) << material.string() << " could not be opened";
    const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    const auto        parsed = Common::Json::Read<MaterialData>( text );
    ASSERT_TRUE( parsed ) << parsed.GetError();

    const uint64_t slot = parsed.GetValue().GetCloudAsset( "CloudType1" );
    ASSERT_NE( slot, 0u ) << material.string() << " no longer authors CloudType1; pick a material that does";

    // The handles the service would hold: one per shipped type, derived as the asset derives it.
    std::map<uint64_t, std::string> registered;
    for ( const auto& entry : std::filesystem::directory_iterator( types ) )
    {
        if ( entry.path().extension() != kCloudTypeExtension )
            continue;
        std::ifstream     file( entry.path() );
        const std::string body( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
        const auto        type = ParseCloudType( body );
        ASSERT_TRUE( type ) << entry.path().string() << ": " << type.GetError();
        ASSERT_TRUE( type.GetValue().Header.has_value() ) << entry.path().string();
        const auto guid = Common::Content::AssetGuidFromText( type.GetValue().Header->Guid );
        ASSERT_TRUE( guid ) << entry.path().string();
        registered[static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) )] =
             entry.path().filename().string();
    }
    ASSERT_EQ( registered.size(), 9u );

    EXPECT_TRUE( registered.count( slot ) )
         << "M_Clouds_Demo_Clouds.demat CloudType1 = " << slot
         << " names no shipped cloud type: every type registers under HandleForGuid of its header GUID, so "
            "CloudTypeService::GetShape misses and the layer silently renders the built-in default. The "
            "slot still holds the pre-format-4 path-derived number; T6c (MATL 3) re-points it by GUID.";
}

// T6b2 (AF7y) - `.dclayout` 1 -> 2: THE LAYOUT IS WRAPPED IN THE AF1 BINARY ENVELOPE, like a `.detex`.
// Written first; RED until T6b2 lands. The header is read by the one BinaryEnvelopeFormat (kind CloudLayout,
// a GUID, the layout version under its own tag) without the painting being decoded, and a bare version-1
// "DCLY" file is refused by name, pointing at Tools/SceneMigrator.
namespace
{
    Desert::Assets::CloudLayoutData SmallMaskOnlyLayout()
    {
        auto canvas = Desert::Assets::MakeCloudLayoutCanvas( Desert::Assets::kCloudLayoutMinResolution );
        EXPECT_TRUE( canvas );
        auto painted = canvas.ExtractValue();
        EXPECT_TRUE( Desert::Assets::SetCloudLayoutCanvasMask( painted, true ) );
        auto made = Desert::Assets::MakeCloudLayoutFromCanvas( painted );
        EXPECT_TRUE( made );
        return made.ExtractValue();
    }
} // namespace

TEST( CloudLayoutFormat, AnEncodedLayoutIsAnAssetEnvelopeWhoseHeaderNamesACloudLayout )
{
    const auto encoded = Desert::Assets::EncodeCloudLayout( SmallMaskOnlyLayout() );
    ASSERT_TRUE( encoded ) << encoded.GetError();
    const auto&                      bytes = encoded.GetValue();
    const std::span<const std::byte> view( reinterpret_cast<const std::byte*>( bytes.data() ), bytes.size() );
    ASSERT_TRUE( Common::Content::BinaryEnvelopeHeaderFormat().Recognises(
         view.first( std::min( view.size(), Common::Content::ASSET_HEADER_SNIFF_BYTES ) ) ) )
         << "a .dclayout does not open with the DAST envelope";
    const auto header =
         Common::Content::ReadEnvelopeHeader( view, Common::Content::AssetHeaderReadContext{ {}, true } );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Asset.Kind, Common::Content::ContentKind::CloudLayout );
    EXPECT_FALSE( header.GetValue().Asset.Guid.IsNull() );
    ASSERT_EQ( header.GetValue().Asset.Subsystems.size(), 1u );
    EXPECT_EQ( header.GetValue().Asset.Subsystems[0].Version, 2u );
    EXPECT_TRUE( Desert::Assets::DecodeCloudLayout( bytes ) ) << "the envelope does not read back";
}

TEST( CloudLayoutFormat, ABareVersionOneFileIsRefusedNamingTheMigrator )
{
    // The version-1 container as it shipped: "DCLY", u32 1, then the 40 bytes the payload header was.
    std::vector<unsigned char> v1 = { 'D', 'C', 'L', 'Y', 1, 0, 0, 0 };
    v1.resize( 48, 0 );
    const auto refused = Desert::Assets::DecodeCloudLayout( v1 );
    ASSERT_FALSE( refused ) << "a header-less version-1 layout was read";
    EXPECT_NE( refused.GetError().find( "version 1" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "SceneMigrator" ), std::string::npos ) << refused.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// FORMAT 3 IS REFUSED BY NAME (AF7v). A v3 file is a complete, legal type with no identity; reading it
// would hand it a handle nothing can name again, so the refusal says what moved and which tool fixes it.
TEST( CloudTypeFormat, AVersionThreeFileWithoutAHeaderIsRefusedNamingTheMigrator )
{
    CloudTypeData data = LegalData();
    std::string   text = WriteCloudType( data );
    const auto    at   = text.find( "\"Header\"" );
    ASSERT_NE( at, std::string::npos );
    const auto close = text.find( "}", text.find( "\"Dependencies\"", at ) );
    text             = R"({"FormatVersion":3,)" + text.substr( text.find_first_not_of( " \t\r\n,", close + 1 ) );

    const auto refused = ParseCloudType( text );
    ASSERT_FALSE( refused ) << "a header-less version-3 file was read";
    EXPECT_NE( refused.GetError().find( "format version 3" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "SceneMigrator" ), std::string::npos ) << refused.GetError();
}

TEST( CloudTypeFormat, ANoiseVolumeIsNamedByGuidAndStatedAsTheOneDependency )
{
    CloudTypeData data = LoadShipped( kCloudTypeCumulusCongestus );
    data.NoiseVolume   = Desert::Assets::AssetGuidRef{ "0123456789abcdef0123456789abcdef", "Clouds/X.dcnv" };
    const std::string written = WriteCloudType( data );
    auto              parsed  = ParseCloudType( written );
    if ( !parsed )
    {
        ADD_FAILURE() << parsed.GetError();
        return;
    }
    const CloudTypeData& parsedType = parsed.GetValue();
    if ( !parsedType.Header )
    {
        ADD_FAILURE() << "the parsed type states no header";
        return;
    }
    EXPECT_EQ( parsedType.NoiseVolume, data.NoiseVolume );
    EXPECT_EQ( parsedType.Header->Dependencies, std::vector<std::string>{ "0123456789abcdef0123456789abcdef" } );

    // The dependency dropped from the header: one reference stated once is refused, by name.
    CloudTypeData unstated = parsed.GetValue();
    if ( !unstated.Header )
    {
        ADD_FAILURE() << "the copied type states no header";
        return;
    }
    unstated.Header->Dependencies.clear();
    const std::string text    = Common::Json::Write( unstated );
    auto              refused = ParseCloudType( text );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "Dependencies" ), std::string::npos ) << refused.GetError();

    // A reference without a GUID is a bare path again.
    CloudTypeData bare = parsed.GetValue();
    if ( !bare.NoiseVolume || !bare.Header )
    {
        ADD_FAILURE() << "the copied type names no volume or states no header";
        return;
    }
    bare.NoiseVolume->Guid.clear();
    bare.Header->Dependencies = { "" };
    auto noGuid               = ParseCloudType( Common::Json::Write( bare ) );
    ASSERT_FALSE( noGuid );
    EXPECT_NE( noGuid.GetError().find( "GUID" ), std::string::npos ) << noGuid.GetError();
}

TEST( CloudTypeFormat, AVersionFourFileIsRefusedByItsVersionAndNotByAJsonTypeError )
{
    const std::string v4 =
         R"({"Header":{"Kind":"CloudType","Guid":"0123456789abcdef0123456789abcdef","Versions":{"CLTY":4},"Dependencies":[]},)"
         R"("NoiseVolume":"Clouds/CloudNoise_FineWisp.dcnv","Shape":{}})";
    auto refused = ParseCloudType( v4 );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "version 4" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "SceneMigrator" ), std::string::npos ) << refused.GetError();
}
