// The cloud material SCHEMA census — DEV_CONTRACT §1.3's fifth link, restated for parameters that are no
// longer reflected C++.
//
// O1 moved thirty-three look fields off ECS::VolumetricCloudData into the Properties block of
// CloudRaymarch.shader (domain Volume). SettingConsumers pins every REFLECTED field to a consumer; the
// moment a value stops being reflected that guarantee lapses, and this suite is where it continues: the
// schema, the C++ mirror that consumes it (Graphic::CloudMaterialValues), and the defaults the two agree
// on are asserted as RELATIONS, in both directions, against the shader file the engine actually loads.
//
// WHY A MIRROR AT ALL, AND WHY IT IS NOT A SECOND SOURCE OF TRUTH. At runtime the schema supplies every
// default (BuildCloudMaterialValues reads the parsed Properties); the struct's member initializers exist
// so a missing shader degrades to the SAME sky loudly rather than to a zeroed one silently. That is only
// safe while the two are byte-equal — which is precisely what this suite pins, so a default retuned in
// the shader without the mirror (or vice versa) is a red test naming the parameter, not a sky that
// quietly forked from its fallback.
//
// Pure: reads one file, runs the engine's own parser, touches no GPU and no registry.

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Clouds/CloudMediumValues.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Core::Formats::ShaderDomain;
using Desert::Core::Formats::ShaderParam;
using Desert::Core::Formats::ShaderProgramMeta;
using Desert::Core::Preprocess::DShaderParser;
using Desert::Graphic::BuildCloudMaterialValues;
using Desert::Graphic::CloudMaterialValues;
using Desert::Graphic::MaterialOverrides;

namespace
{
    // Walks up from the working directory looking for a file only the repository has — the test runner's
    // working directory is not fixed. Same shape as CloudProtocolScene's RepoRoot, same reason.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::string& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // The MATL 2 writer pretty-prints (indent + "Key": value); membership by text is about the JSON's
    // tokens, not its layout, so whitespace outside string literals is dropped before any find.
    std::string CompactJson( const std::string& json )
    {
        std::string out;
        out.reserve( json.size() );
        bool inString = false;
        bool escaped  = false;
        for ( const char c : json )
        {
            if ( inString )
            {
                out += c;
                if ( escaped )
                    escaped = false;
                else if ( c == '\\' )
                    escaped = true;
                else if ( c == '"' )
                    inString = false;
                continue;
            }
            if ( c == '"' )
                inString = true;
            if ( std::isspace( static_cast<unsigned char>( c ) ) == 0 )
                out += c;
        }
        return out;
    }

    // Parsed ONCE for the whole suite: the file on disk is the thing under test, and every test reads
    // the same parse of it.
    const ShaderProgramMeta& Schema()
    {
        static const ShaderProgramMeta meta = []
        {
            const std::string path = RepoRoot() + "Editor/Resources/Shaders/Programs/Clouds/CloudRaymarch.shader";
            const std::string source = ReadAll( path );
            EXPECT_FALSE( source.empty() ) << path << " is unreadable";

            auto parsed = DShaderParser::Parse( source );
            EXPECT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
            return parsed.IsSuccess() ? parsed.GetValue().Meta : ShaderProgramMeta{};
        }();
        return meta;
    }

    const ShaderParam* Find( const char* name )
    {
        for ( const auto& p : Schema().Params )
            if ( p.Name == name )
                return &p;
        return nullptr;
    }

    // THE MOVED THIRTY-THREE, spelled once, with the storage each one mirrors. This table is the census:
    // both directions of the schema<->struct correspondence are asserted against it, so a parameter added
    // to either side alone is a red test naming the name.
    struct ValueRow
    {
        const char* Name;
        int         Components; // 1 scalar, 2/3 vector
        bool        IsInt;
    };
    constexpr ValueRow kValues[] = {
         { "Coverage", 1, false },
         { "CoverageContrast", 1, false },
         { "WeatherTileSize", 1, false },
         { "Seed", 1, true },
         { "PlacementDensity", 1, false },
         { "PlacementScatter", 1, false },
         { "PlacementSizeVariety", 1, false },
         { "PatchTileSize", 1, false },
         { "PatchStrength", 1, false },
         { "LayoutPatternStrength", 1, false },
         { "LayoutMaskStrength", 1, false },
         { "LayoutRepeats", 1, true },
         { "LayoutRotation", 1, true },
         { "LayoutOffset", 2, false },
         { "DetailTileSize", 1, false },
         { "DetailStrength", 1, false },
         { "DensityScale", 1, false },
         { "ExtinctionScale", 1, false },
         // THREE, since the Volume domain's output contract landed: the scattering albedo is the medium's
         // own colour. A `.demat` written while it was one is raised by
         // Migration::MigrateCloudMaterialAlbedoToColour, not papered over by the reader.
         { "ScatteringAlbedo", 3, false },
         { "PhaseG", 1, false },
         { "PhaseGBackward", 1, false },
         { "PhaseBlend", 1, false },
         { "AmbientOcclusionStrength", 1, false },
         { "MultiScatterOctaves", 1, true },
         { "MultiScatterContribution", 1, false },
         { "MultiScatterOcclusion", 1, false },
         { "MultiScatterEccentricity", 1, false },
         { "AmbientScale", 3, false },
    };
    struct AssetRow
    {
        const char* Name;
        const char* Kind;
    };
    constexpr AssetRow kAssets[] = {
         { "CloudType1", "CloudTypeAsset" },
         { "CloudType2", "CloudTypeAsset" },
         { "CloudType3", "CloudTypeAsset" },
         { "CloudType4", "CloudTypeAsset" },
         // TWO LAYOUT INPUTS AND NOT ONE (O-4), which is Unreal's own arrangement:
         // `Layout_CloudGlobalPattern` and `Layout_GlobalCloudMask` are separate texture parameters
         // there. A census that still named the single `CloudLayout` would pass on a shader that had
         // quietly lost the mask input.
         { "LayoutPattern", "CloudLayoutAsset" },
         { "LayoutMask", "CloudLayoutAsset" },
         // THE AUTHORED MEDIUM, and it is an asset reference rather than a value because what it names is
         // a body of CODE: a Volume-domain shader graph that replaces the density, extinction, albedo,
         // emission and occlusion of the medium itself. It reaches the frame through the shader compiler
         // — Core::ShaderVariant, substituted into the four programs that sample the cloud field — and
         // never through the packed parameter block, which is why it costs the march nothing per sample.
         // Null is the shipped medium.
         { "Medium", "ShaderAsset" },
    };
} // namespace

TEST( CloudMaterialSchema, TheShaderIsTheVolumeDomainAndItsNameIsTheSharedConstant )
{
    EXPECT_EQ( Schema().Domain, ShaderDomain::Volume )
         << "the cloud shader stopped declaring Domain Volume, so no material picker offers it and the "
            "cloud component's slot has nothing to point at";

    // The one spelling: the renderer's pipeline lookup, the schema resolve and the layout panel all use
    // this constant, and the file on disk has to be the program it names.
    EXPECT_STREQ( Desert::Graphic::kCloudMaterialShaderName, "CloudRaymarch" );
}

TEST( CloudMaterialSchema, TheSchemaAndTheMirrorAgreeInBothDirections )
{
    // Every table row exists in the schema, with the storage the mirror expects.
    for ( const ValueRow& row : kValues )
    {
        const ShaderParam* p = Find( row.Name );
        ASSERT_NE( p, nullptr ) << row.Name << " is in CloudMaterialValues but not in the schema — the "
                                << "material editor cannot author it";
        EXPECT_FALSE( p->IsTexture ) << row.Name;
        EXPECT_FALSE( p->IsAssetRef() ) << row.Name;
        using VT = Desert::Core::Formats::ShaderValueType;
        if ( row.IsInt )
            EXPECT_EQ( p->Type, VT::Int ) << row.Name;
        else if ( row.Components == 2 )
            EXPECT_EQ( p->Type, VT::Float2 ) << row.Name;
        else if ( row.Components == 3 )
            EXPECT_EQ( p->Type, VT::Float3 ) << row.Name;
        else
            EXPECT_EQ( p->Type, VT::Float ) << row.Name;
    }
    for ( const AssetRow& row : kAssets )
    {
        const ShaderParam* p = Find( row.Name );
        ASSERT_NE( p, nullptr ) << row.Name;
        EXPECT_EQ( p->AssetKind, row.Kind ) << row.Name;
    }

    // And the schema carries NOTHING the mirror does not read: a parameter here that no C++ consumes is
    // §1.3's dead setting, wearing the new clothes.
    std::set<std::string> known;
    for ( const ValueRow& row : kValues )
        known.insert( row.Name );
    for ( const AssetRow& row : kAssets )
        known.insert( row.Name );

    EXPECT_EQ( Schema().Params.size(), known.size() );
    for ( const auto& p : Schema().Params )
        EXPECT_TRUE( known.count( p.Name ) )
             << p.Name << " is in the schema but not in CloudMaterialValues — a knob nothing reads";
}

TEST( CloudMaterialSchema, TheSchemaDefaultsAreTheMirrorsToTheDigit )
{
    // The mirror at its member initializers IS the schema's defaults — asserted value by value rather
    // than through BuildCloudMaterialValues, so a divergence names the parameter instead of the pair.
    const CloudMaterialValues mirror{};

    const auto def = []( const char* name ) { return Find( name )->Default; };

    EXPECT_FLOAT_EQ( def( "Coverage" ).x, mirror.Coverage );
    EXPECT_FLOAT_EQ( def( "CoverageContrast" ).x, mirror.CoverageContrast );
    EXPECT_FLOAT_EQ( def( "WeatherTileSize" ).x, mirror.WeatherTileSize );
    EXPECT_EQ( static_cast<int32_t>( def( "Seed" ).x ), mirror.Seed );
    EXPECT_FLOAT_EQ( def( "PlacementDensity" ).x, mirror.PlacementDensity );
    EXPECT_FLOAT_EQ( def( "PlacementScatter" ).x, mirror.PlacementScatter );
    EXPECT_FLOAT_EQ( def( "PlacementSizeVariety" ).x, mirror.PlacementSizeVariety );
    EXPECT_FLOAT_EQ( def( "PatchTileSize" ).x, mirror.PatchTileSize );
    EXPECT_FLOAT_EQ( def( "PatchStrength" ).x, mirror.PatchStrength );
    EXPECT_FLOAT_EQ( def( "LayoutPatternStrength" ).x, mirror.LayoutPatternStrength );
    EXPECT_FLOAT_EQ( def( "LayoutMaskStrength" ).x, mirror.LayoutMaskStrength );
    EXPECT_EQ( static_cast<int32_t>( def( "LayoutRepeats" ).x ), mirror.LayoutRepeats );
    EXPECT_EQ( static_cast<int32_t>( def( "LayoutRotation" ).x ), mirror.LayoutRotation );
    EXPECT_FLOAT_EQ( def( "LayoutOffset" ).x, mirror.LayoutOffset.x );
    EXPECT_FLOAT_EQ( def( "LayoutOffset" ).y, mirror.LayoutOffset.y );
    EXPECT_FLOAT_EQ( def( "DetailTileSize" ).x, mirror.DetailTileSize );
    EXPECT_FLOAT_EQ( def( "DetailStrength" ).x, mirror.DetailStrength );
    EXPECT_FLOAT_EQ( def( "DensityScale" ).x, mirror.DensityScale );
    EXPECT_FLOAT_EQ( def( "ExtinctionScale" ).x, mirror.ExtinctionScale );
    EXPECT_FLOAT_EQ( def( "ScatteringAlbedo" ).x, mirror.ScatteringAlbedo.x );
    EXPECT_FLOAT_EQ( def( "ScatteringAlbedo" ).y, mirror.ScatteringAlbedo.y );
    EXPECT_FLOAT_EQ( def( "ScatteringAlbedo" ).z, mirror.ScatteringAlbedo.z );
    EXPECT_FLOAT_EQ( def( "PhaseG" ).x, mirror.PhaseG );
    EXPECT_FLOAT_EQ( def( "PhaseGBackward" ).x, mirror.PhaseGBackward );
    EXPECT_FLOAT_EQ( def( "PhaseBlend" ).x, mirror.PhaseBlend );
    EXPECT_FLOAT_EQ( def( "AmbientOcclusionStrength" ).x, mirror.AmbientOcclusionStrength );
    EXPECT_EQ( static_cast<int32_t>( def( "MultiScatterOctaves" ).x ), mirror.MultiScatterOctaves );
    EXPECT_FLOAT_EQ( def( "MultiScatterContribution" ).x, mirror.MultiScatterContribution );
    EXPECT_FLOAT_EQ( def( "MultiScatterOcclusion" ).x, mirror.MultiScatterOcclusion );
    EXPECT_FLOAT_EQ( def( "MultiScatterEccentricity" ).x, mirror.MultiScatterEccentricity );
    EXPECT_FLOAT_EQ( def( "AmbientScale" ).x, mirror.AmbientScale.x );
    EXPECT_FLOAT_EQ( def( "AmbientScale" ).y, mirror.AmbientScale.y );
    EXPECT_FLOAT_EQ( def( "AmbientScale" ).z, mirror.AmbientScale.z );
}

TEST( CloudMaterialSchema, EveryValueParameterHasItsSliderAndItsProse )
{
    for ( const auto& p : Schema().Params )
    {
        EXPECT_FALSE( p.Category.empty() ) << p.Name << " has no Category, so it lands in an unnamed group";
        EXPECT_FALSE( p.Tooltip.empty() ) << p.Name << " has no Tooltip — the component fields these "
                                          << "replaced all documented themselves, and the material must not "
                                          << "author blinder than the Details panel did";
        if ( p.IsAssetRef() )
            continue;
        using VT = Desert::Core::Formats::ShaderValueType;
        if ( p.Type == VT::Float || p.Type == VT::Int )
            EXPECT_TRUE( p.Min.has_value() && p.Max.has_value() )
                 << p.Name << " has no Range, so it draws as a bare drag field";
    }

    // The three copies of the octave ceiling stayed one number through the move: the schema's Range, the
    // packer's clamp and the shader's own loop bound all answer to kCloudMultiScatterMaxOctaves.
    EXPECT_FLOAT_EQ( Find( "MultiScatterOctaves" )->Max.value_or( 0.0f ),
                     static_cast<float>( Desert::ECS::kCloudMultiScatterMaxOctaves ) );

    // And every default sits inside its own slider — the relation ComponentReflection asserts for the
    // component's fields, continued for the schema's.
    for ( const auto& p : Schema().Params )
    {
        if ( !p.Min.has_value() || !p.Max.has_value() )
            continue;
        EXPECT_GE( p.Default.x, *p.Min ) << p.Name << " defaults below its own slider";
        EXPECT_LE( p.Default.x, *p.Max ) << p.Name << " defaults above its own slider";
    }
}

TEST( CloudMaterialSchema, BuildAppliesSchemaThenOverridesAndSkipsWhatItDoesNotKnow )
{
    // Defaults path: schema in, mirror out — the two agreeing is the previous test; this one asserts the
    // BUILDER takes them from the schema (a schema default perturbed in-memory must come through).
    ShaderProgramMeta perturbed = Schema();
    for ( auto& p : perturbed.Params )
        if ( p.Name == "Coverage" )
            p.Default.x = 0.77f;

    const CloudMaterialValues fromSchema = BuildCloudMaterialValues( &perturbed, MaterialOverrides{} );
    EXPECT_FLOAT_EQ( fromSchema.Coverage, 0.77f )
         << "the builder ignored the schema's default — the shader file is not the source of truth";

    // Overrides land last and win; asset references ride the name->handle map; an unknown name is
    // SKIPPED rather than guessed at (a .demat may be ahead of or behind this binary).
    MaterialOverrides overrides;
    overrides.Params.emplace_back( "Coverage", glm::vec4( 0.25f, 0.0f, 0.0f, 0.0f ) );
    overrides.Params.emplace_back( "AmbientScale", glm::vec4( 0.5f, 0.25f, 0.125f, 0.0f ) );
    overrides.Params.emplace_back( "MultiScatterOctaves", glm::vec4( 2.0f, 0.0f, 0.0f, 0.0f ) );
    overrides.Params.emplace_back( "NotAKnownParameter", glm::vec4( 42.0f ) );
    overrides.Textures.emplace_back( "CloudType3", 0xBEEFull );
    overrides.Textures.emplace_back( "LayoutPattern", 0xCAFEull );
    overrides.Textures.emplace_back( "LayoutMask", 0xFEEDull );
    overrides.Textures.emplace_back( "NotAKnownSlot", 0xDEADull );

    const CloudMaterialValues values = BuildCloudMaterialValues( &Schema(), overrides );
    EXPECT_FLOAT_EQ( values.Coverage, 0.25f );
    EXPECT_EQ( values.AmbientScale, glm::vec3( 0.5f, 0.25f, 0.125f ) );
    EXPECT_EQ( values.MultiScatterOctaves, 2 );
    EXPECT_EQ( static_cast<uint64_t>( values.CloudType3 ), 0xBEEFull );
    EXPECT_EQ( static_cast<uint64_t>( values.LayoutPattern ), 0xCAFEull );
    EXPECT_EQ( static_cast<uint64_t>( values.LayoutMask ), 0xFEEDull )
         << "the pattern and the mask are separate inputs; a reader that folded them together would "
            "pass this line with one of the two handles in both fields";
    // Untouched neighbours keep the schema defaults.
    EXPECT_FLOAT_EQ( values.CoverageContrast, CloudMaterialValues{}.CoverageContrast );
    EXPECT_EQ( static_cast<uint64_t>( values.CloudType1 ), 0ull );

    // No schema at all is the loud-degradation path: the mirror stands in, which the digit-equality test
    // above proves is the same sky.
    const CloudMaterialValues noSchema = BuildCloudMaterialValues( nullptr, overrides );
    EXPECT_FLOAT_EQ( noSchema.Coverage, 0.25f );
}

// THE ALBEDO IS READ AS THREE COMPONENTS AND NOT REPAIRED ON THE WAY IN — which is what makes the
// migration necessary rather than optional, and it is asserted here so nobody makes the reader "helpful".
//
// The tempting leniency is "if y and z are zero, broadcast x": it would make every unmigrated `.demat`
// render correctly. It is refused twice over. It makes (0.98, 0, 0) — a legal authored colour now that the
// slot has three components — inexpressible; and it hides an unraised file for ever, so the corpus would
// carry two shapes of the same value indefinitely and the next person to touch either end would meet both.
// Migration::MigrateCloudMaterialAlbedoToColour raises the file ONCE instead, and the SceneCloudMaterial-
// Migration suite is where that is tested.
// ── A NUMBER THE RENDERER CANNOT READ IS REFUSED BY NAME, FOR EVERY PARAMETER ─────────────────────────
//
// THE DEFECT, AND WHY IT IS ONE TEST OVER THE WHOLE TABLE RATHER THAN FOUR ASSERTIONS. `std::clamp` is
// `v < lo ? lo : hi < v ? hi : v`; every comparison against a NaN is false; so a NaN comes out of a clamp
// unchanged. Task O11 found that on one knob and guarded that knob. O13 measured the same shape on four
// more in Graphic::ApplyCloudMaterialToBakeParams — Coverage, PlacementDensity, PlacementScatter,
// PatchStrength — and the medium resolver has no clamp on its path at all. Guarding them one at a time is
// how a subsystem ends up with the fifth one unguarded, so the guard moved to the single seam every value
// enters through (Detail::AssignCloudValue over Graphic::MaterialValueIsReadable), and this is that seam
// asserted OVER THE WHOLE CENSUS TABLE: a parameter added to kValues is a parameter this test covers on
// the same day, with no second list to remember.
//
// THE THREE VALUES ARE NOT DECORATIVE. A NaN is the one a clamp passes through; the two infinities are the
// ones that survive a `std::max` floor and reach a division; and for the four integer parameters a finite
// 1e30 is just as undefined as any of them, because `static_cast<int32_t>` of an out-of-range float is UB
// BEFORE any range check downstream can look at it.
//
// WHAT REFUSAL MEANS HERE: the field keeps what the schema gave it. That is asserted as "the built look is
// the schema-only look, field for field" rather than as "the field is not NaN", because the second would
// pass on a reader that substituted a zero — which is a different sky, silently.
namespace
{
    // The whole look compared field by field. It exists because the struct has padding and a memcmp of two
    // separately constructed ones can differ in bytes nobody reads; and because a mismatch has to NAME the
    // field, or a red line says only "something moved".
    void ExpectSameLook( const CloudMaterialValues& got, const CloudMaterialValues& want,
                         const std::string& context )
    {
#define O14_SAME( field ) EXPECT_EQ( got.field, want.field ) << context << ": " << #field << " moved"
        O14_SAME( CloudType1 );
        O14_SAME( CloudType2 );
        O14_SAME( CloudType3 );
        O14_SAME( CloudType4 );
        O14_SAME( Coverage );
        O14_SAME( CoverageContrast );
        O14_SAME( WeatherTileSize );
        O14_SAME( Seed );
        O14_SAME( PlacementDensity );
        O14_SAME( PlacementScatter );
        O14_SAME( PlacementSizeVariety );
        O14_SAME( PatchTileSize );
        O14_SAME( PatchStrength );
        O14_SAME( LayoutPattern );
        O14_SAME( LayoutMask );
        O14_SAME( LayoutPatternStrength );
        O14_SAME( LayoutMaskStrength );
        O14_SAME( LayoutRepeats );
        O14_SAME( LayoutRotation );
        O14_SAME( LayoutOffset );
        O14_SAME( DetailTileSize );
        O14_SAME( DetailStrength );
        O14_SAME( DensityScale );
        O14_SAME( ExtinctionScale );
        O14_SAME( ScatteringAlbedo );
        O14_SAME( PhaseG );
        O14_SAME( PhaseGBackward );
        O14_SAME( PhaseBlend );
        O14_SAME( AmbientOcclusionStrength );
        O14_SAME( MultiScatterOctaves );
        O14_SAME( MultiScatterContribution );
        O14_SAME( MultiScatterOcclusion );
        O14_SAME( MultiScatterEccentricity );
        O14_SAME( AmbientScale );
        O14_SAME( Medium );
#undef O14_SAME
    }
} // namespace

TEST( CloudMaterialSchema, NoParameterCanCarryANumberTheRendererCannotRead )
{
    const CloudMaterialValues schemaOnly = BuildCloudMaterialValues( &Schema(), MaterialOverrides{} );

    // Every one of them is finite to begin with — otherwise the test below would be asserting that a
    // broken default equals itself.
    ASSERT_TRUE( std::isfinite( schemaOnly.Coverage ) );
    ASSERT_TRUE( std::isfinite( schemaOnly.ExtinctionScale ) );

    const float kNaN  = std::numeric_limits<float>::quiet_NaN();
    const float kInf  = std::numeric_limits<float>::infinity();
    const float kHuge = 1e30f; // finite, and still undefined as an int32

    std::size_t lanesCovered = 0;
    std::size_t casesRun     = 0;

    for ( const ValueRow& row : kValues )
    {
        std::vector<float> unreadable = { kNaN, kInf, -kInf };
        if ( row.IsInt )
            unreadable.push_back( kHuge );

        for ( int lane = 0; lane < row.Components; ++lane )
        {
            ++lanesCovered;
            for ( const float bad : unreadable )
            {
                // The OTHER lanes stay finite on purpose: the claim is that the reader refuses the
                // parameter because of the lane it is about to read, not because something else in the
                // vec4 happened to be odd.
                glm::vec4 value( 0.5f, 0.5f, 0.5f, 0.5f );
                value[lane] = bad;

                MaterialOverrides overrides;
                overrides.Params.emplace_back( row.Name, value );

                ++casesRun;
                ExpectSameLook( BuildCloudMaterialValues( &Schema(), overrides ), schemaOnly,
                                std::string( row.Name ) + " lane " + std::to_string( lane ) + " = " +
                                     std::to_string( bad ) );
            }
        }
    }

    // DERIVED AND PRINTED, NOT PINNED. A number in the source could be edited to match a census that had
    // stopped looking; what is asserted is that the table was not empty and that every lane of it ran.
    EXPECT_EQ( lanesCovered, 33u ) << "the census table's lane count moved — read kValues, not this number";
    std::cout << "[ CENSUS   ] cloud material value parameters: " << std::size( kValues ) << ", lanes "
              << lanesCovered << ", unreadable cases refused: " << casesRun << std::endl;

    // THE NEGATIVE CONTROL. A reader that refused everything would pass every line above.
    MaterialOverrides good;
    good.Params.emplace_back( "Coverage", glm::vec4( 0.33f, 0.0f, 0.0f, 0.0f ) );
    good.Params.emplace_back( "MultiScatterOctaves", glm::vec4( 4.0f, 0.0f, 0.0f, 0.0f ) );
    good.Params.emplace_back( "AmbientScale", glm::vec4( 0.25f, 0.5f, 0.75f, 0.0f ) );
    const CloudMaterialValues readable = BuildCloudMaterialValues( &Schema(), good );
    EXPECT_FLOAT_EQ( readable.Coverage, 0.33f );
    EXPECT_EQ( readable.MultiScatterOctaves, 4 );
    EXPECT_EQ( readable.AmbientScale, glm::vec3( 0.25f, 0.5f, 0.75f ) );
}

// ── THE BOUNDARY: WHAT EVERY CLAMP DOWNSTREAM READS IS SOMETHING THE ENTRY ALREADY REFUSED ────────────
//
// THE QUESTION THIS ANSWERS, asked because "four clamps pass a NaN" was the brief and four was not the
// number. A clamp does not repair a non-finite value, so the honest count is "how many places in the tree
// clamp a cloud material value and would therefore hand a NaN onward". It is DERIVED here rather than
// written down: the source is scanned for `std::clamp` / `std::max` / `std::min` applied to a field of
// CloudMaterialValues, and the field names are collected.
//
// THE RELATION, and it is what makes this a test rather than a statistic: every field any of those sites
// reads must be a field the entry guard covers, i.e. a row of kValues. Then "the clamp cannot see a
// non-finite value" is a property of the pair rather than a hope about either half, and a parameter that
// is clamped somewhere but is not a census row reddens this instead of being the one nobody guarded.
//
// WHY THE GUARD IS NOT ALSO PUT AT EACH SITE: because there are thirty-one of them across three files and
// the medium path has none at all — that is precisely the "fifteen places of one shape" the abstraction
// replaces. Fixing them one at a time is how the thirty-second arrives unguarded.
namespace
{
    std::string RepoRootForBoundary()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/Clouds/CloudMaterialBake.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }
} // namespace

TEST( CloudMaterialSchema, EveryCloudMaterialValueAClampReadsIsOneTheEntryRefuses )
{
    namespace fs = std::filesystem;

    const std::string root = RepoRootForBoundary();
    ASSERT_FALSE( root.empty() );

    // The three spellings the readers give a CloudMaterialValues: the bake's parameter, the packer's, and
    // the renderer's member. Typed, and small enough to audit by eye; what is DERIVED is where they occur.
    const std::vector<std::string> receivers = { "look.", "material.", "m_Material.", "values." };
    const std::vector<std::string> guards    = { "std::clamp(", "std::max(", "std::min(" };

    std::set<std::string> clampedFields;
    std::size_t           sites = 0;

    std::vector<fs::path> sources;
    for ( const char* tree : { "Desert/Desert/Source/Engine/Graphic", "Editor/Source/Editor/Panels/Clouds" } )
    {
        std::error_code ec;
        for ( auto it = fs::recursive_directory_iterator( fs::path( root ) / tree, ec );
              !ec && it != fs::recursive_directory_iterator(); ++it )
            if ( it->path().extension() == ".cpp" || it->path().extension() == ".hpp" )
                sources.push_back( it->path() );
    }
    ASSERT_FALSE( sources.empty() );

    for ( const fs::path& path : sources )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::string src = buffer.str();
        // The prose in these files quotes the very expressions being counted, so comments have to go or
        // the census counts itself. Whitespace inside the call is squeezed out for the same reason the
        // receivers are typed: `std::clamp( look.X` and `std::clamp(look.X` are one site.
        src.erase( std::remove( src.begin(), src.end(), ' ' ), src.end() );

        std::string stripped;
        stripped.reserve( src.size() );
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src.compare( i, 2, "//" ) == 0 )
            {
                while ( i < src.size() && src[i] != '\n' )
                    ++i;
                continue;
            }
            if ( src.compare( i, 2, "/*" ) == 0 )
            {
                i += 2;
                while ( i + 1 < src.size() && src.compare( i, 2, "*/" ) != 0 )
                    ++i;
                i += 2;
                continue;
            }
            stripped.push_back( src[i++] );
        }

        for ( const std::string& guard : guards )
        {
            std::string squeezedGuard = guard;
            squeezedGuard.erase( std::remove( squeezedGuard.begin(), squeezedGuard.end(), ' ' ),
                                 squeezedGuard.end() );
            for ( std::size_t at = stripped.find( squeezedGuard ); at != std::string::npos;
                  at             = stripped.find( squeezedGuard, at + 1 ) )
            {
                const std::size_t argAt = at + squeezedGuard.size();
                for ( const std::string& receiver : receivers )
                {
                    if ( stripped.compare( argAt, receiver.size(), receiver ) != 0 )
                        continue;
                    std::size_t end = argAt + receiver.size();
                    while (
                         end < stripped.size() &&
                         ( std::isalnum( static_cast<unsigned char>( stripped[end] ) ) || stripped[end] == '_' ) )
                        ++end;
                    const std::string field =
                         stripped.substr( argAt + receiver.size(), end - argAt - receiver.size() );
                    if ( field.empty() )
                        continue;
                    ++sites;
                    clampedFields.insert( field );
                }
            }
        }
    }

    // Derived and printed. The number is evidence, not a gate: a gate on it could be satisfied by editing
    // it, and the thing that must hold is the relation below.
    std::cout << "[ CENSUS   ] clamp/max/min sites over a cloud material value: " << sites << ", distinct "
              << "fields: " << clampedFields.size() << std::endl;
    EXPECT_GT( sites, 0u ) << "the boundary census found no clamped material value — it has stopped looking";

    std::set<std::string> guarded;
    for ( const ValueRow& row : kValues )
        guarded.insert( row.Name );

    for ( const std::string& field : clampedFields )
        EXPECT_TRUE( guarded.count( field ) != 0 )
             << "CloudMaterialValues::" << field
             << " is clamped somewhere downstream but is not a census "
                "row, so nothing refuses a non-finite value for it at the entry — a clamp will pass it "
                "straight through to the GPU";
}

// The medium's own resolver, which has NO clamp anywhere on its path: the vec4 is copied into the
// parameter block whole and the shader decides which lanes it reads. All four lanes are therefore the
// question, and a refused property keeps the schema default the graph author typed.
TEST( CloudMediumValues, APropertyThatCannotBeReadKeepsTheGraphsOwnDefault )
{
    std::vector<ShaderParam> schema;
    {
        ShaderParam tint;
        tint.Name    = "Tint";
        tint.Default = glm::vec4( 0.2f, 0.4f, 0.6f, 1.0f );
        schema.push_back( tint );
    }

    const std::string key = Desert::Core::CloudMediumOverrideKey( "Tint" );

    for ( int lane = 0; lane < 4; ++lane )
    {
        glm::vec4 bad( 0.9f, 0.9f, 0.9f, 0.9f );
        bad[lane] = std::numeric_limits<float>::quiet_NaN();

        MaterialOverrides overrides;
        overrides.Params.emplace_back( key, bad );

        const auto values = Desert::Graphic::BuildCloudMediumValues( schema, overrides );
        ASSERT_EQ( values.Params.size(), 1u );
        EXPECT_EQ( values.Params[0], glm::vec4( 0.2f, 0.4f, 0.6f, 1.0f ) )
             << "lane " << lane << " was unreadable and the property did not fall back to the schema";
    }

    // Negative control: a readable override still wins.
    MaterialOverrides good;
    good.Params.emplace_back( key, glm::vec4( 1.0f, 0.0f, 0.0f, 0.5f ) );
    const auto values = Desert::Graphic::BuildCloudMediumValues( schema, good );
    ASSERT_EQ( values.Params.size(), 1u );
    EXPECT_EQ( values.Params[0], glm::vec4( 1.0f, 0.0f, 0.0f, 0.5f ) );
}

TEST( CloudMaterialSchema, TheAlbedoIsAColourAndAnOldScalarIsNotQuietlyRepaired )
{
    MaterialOverrides scalarAsWritten;
    scalarAsWritten.Params.emplace_back( "ScatteringAlbedo", glm::vec4( 0.98f, 0.0f, 0.0f, 0.0f ) );

    const CloudMaterialValues stale = BuildCloudMaterialValues( &Schema(), scalarAsWritten );
    EXPECT_EQ( stale.ScatteringAlbedo, glm::vec3( 0.98f, 0.0f, 0.0f ) )
         << "the reader broadcast a scalar albedo into a colour. That makes an authored (0.98, 0, 0) "
            "impossible to express and lets an unmigrated .demat live for ever; the migrator raises the "
            "file instead.";

    MaterialOverrides authored;
    authored.Params.emplace_back( "ScatteringAlbedo", glm::vec4( 0.9f, 0.72f, 0.55f, 0.0f ) );
    EXPECT_EQ( BuildCloudMaterialValues( &Schema(), authored ).ScatteringAlbedo, glm::vec3( 0.9f, 0.72f, 0.55f ) );
}

// THE PROTOCOL SCENES' MATERIALS ARE FULLY EXPLICIT, which is the §PR instrument-property continued
// across the seam: those scenes exist so no default change can move a measurement, and after O1 a schema
// default could move one through a material that omitted a parameter. The migration writes every stated
// field; this is what keeps somebody from later "cleaning up" the redundant-looking values.
TEST( CloudMaterialSchema, TheProtocolScenesMaterialsStateEveryValueParameter )
{
    const char* const kProtocolMaterials[] = {
         "Materials/M_Clouds_Protocol_Clouds.demat",
         "Materials/M_PR_Hero0_Clouds.demat",
         "Materials/M_PR_Hero3_Clouds.demat",
         "Materials/M_PR_Hero8_Clouds.demat",
    };

    for ( const char* rel : kProtocolMaterials )
    {
        const std::string path = RepoRoot() + "Editor/Resources/Assets/" + rel;
        const std::string json = ReadAll( path );
        ASSERT_FALSE( json.empty() ) << path << " is missing — the protocol scene's look is exposed to "
                                     << "schema defaults again";

        // Membership by name is enough here (the values are the scene author's, not this suite's);
        // parsing the JSON with the engine's reflectors would drag half the engine into a parser suite.
        for ( const ValueRow& row : kValues )
            EXPECT_NE( json.find( std::string( "\"" ) + row.Name + "\"" ), std::string::npos )
                 << rel << " does not state " << row.Name << " — a schema default can now move the protocol's sky";
    }
}

// THE SHARED DEFAULT MATERIAL (D-37, teamlead 2026-09-06) MUST RESOLVE TO THE SAME SKY AS AN EMPTY
// SLOT USED TO — that is the whole point of routing every look-less scene at it instead of leaving
// Material null. The relation is proved by composing three already-separately-tested facts rather than
// re-parsing the file with the full engine (this suite deliberately links nothing past the shader
// parser and Common — see the premake5.lua comment):
//
//   1. TheSchemaDefaultsAreTheMirrorsToTheDigit: CloudMaterialValues{} == the schema's own defaults,
//      digit for digit.
//   2. BuildAppliesSchemaThenOverridesAndSkipsWhatItDoesNotKnow: BuildCloudMaterialValues(schema, {})
//      with an EMPTY MaterialOverrides returns exactly the schema's defaults — overrides only ever
//      move a value away from the default, never toward a second one.
//   3. THIS TEST: M_CloudDefault.demat states no Params and no Textures, which is exactly what an
//      empty MaterialOverrides looks like once loaded — so its resolution is (1) composed with (2),
//      with no numbers copied into the file for a future schema edit to fall out of step with.
//
// A file that stated even one baked-in value here would reintroduce the second-source-of-truth defect
// D-37 exists to remove: the DAY the schema's own default changes, a baked copy stops matching it
// silently, while an empty-overrides file tracks the schema by construction and cannot.
TEST( CloudMaterialSchema, TheSharedDefaultMaterialStatesNoOverridesAndSoCannotDriftFromTheSchema )
{
    const std::string path = RepoRoot() + "Editor/Resources/Assets/Materials/M_CloudDefault.demat";
    const std::string json = CompactJson( ReadAll( path ) );
    ASSERT_FALSE( json.empty() ) << path << " is missing — every scene the migration points at it "
                                 << "(D-37) fails to resolve a material at load";

    // Membership by text, on the same terms TheProtocolScenesMaterialsStateEveryValueParameter uses for
    // the opposite claim (states every value): this file must state NONE.
    EXPECT_NE( json.find( R"("Path":"engine:Shaders/Programs/Clouds/CloudRaymarch.shader")" ), std::string::npos )
         << path << " does not name the Volume-domain cloud shader";
    EXPECT_NE( json.find( R"("Params":[])" ), std::string::npos )
         << path << " states a Param — it must defer to the schema's own default instead of copying it";
    EXPECT_NE( json.find( R"("Textures":[])" ), std::string::npos )
         << path << " states a Texture — it must defer to the schema's own default instead of copying it";
}

// ---------------------------------------------------------------------------------------------------
// WHICH PARAMETERS COST SECONDS — MOVED, AND WHY THE MOVE IS THE POINT (O8-3, O1)
// ---------------------------------------------------------------------------------------------------
//
// THE COMPLAINT THIS ANSWERS, in the owner's words, twice: "I'd like the clouds in the preview to update
// straight away". Half of them already do. A cloud material's parameters fall into two classes with
// completely different costs — read by the CPU BAKE, or read by the MARCH — and until O1's timing pass
// nothing on screen distinguished them.
//
// THAT CLAIM USED TO BE PINNED HERE, BY READING THE RENDERER AS TEXT: the body of
// VolumetricCloudRenderer::BuildProceduralParams was brace-matched out of the .cpp and searched for
// `m_Material.<Name>`. It was the weakest form of guard in this subsystem, and it had the failure mode a
// text guard always has — the bake's material half moved into a header
// (Graphic::ApplyCloudMaterialToBakeParams, so that a suite could CALL it), every `m_Material.` read left
// this file's field of view at once, and the test would have gone red naming twenty parameters with
// nothing wrong with any of them.
//
// It is now Desert/Tests/Engine/CloudMaterialTiming, where each parameter is PERTURBED and the renderer's
// own rebake decision (Assets::CloudProceduralParamsEqual) is asked whether the volume has to be built
// again — the relation executed instead of grepped, and checked against the `Timing` attribute the shader
// now declares and the Material Editor now prints.
//
// WHAT STAYS HERE is the half that belongs to the SCHEMA rather than to the bake: every parameter carries
// the attribute at all. Without it the panel's heading has nothing to say, and a schema census is exactly
// where "a value nobody classified" has to be caught — this suite is the one that walks the Properties
// block.
TEST( CloudMaterialSchema, EveryParameterDeclaresWhenItsEditBecomesVisible )
{
    using Timing = ::Desert::Core::Formats::ShaderParamTiming;

    uint32_t rebake    = 0;
    uint32_t immediate = 0;

    for ( const ShaderParam& p : Schema().Params )
    {
        EXPECT_NE( p.Timing, Timing::Unspecified )
             << p.Name
             << " declares no Timing, so the Material Editor cannot tell an artist whether moving "
                "it costs a frame or fourteen seconds. Add Timing(Immediate) or Timing(Rebake) to "
                "its Properties line; CloudMaterialTiming then checks the one you chose against "
                "the bake itself.";

        if ( p.Timing == Timing::Rebake )
            ++rebake;
        else if ( p.Timing == Timing::Immediate )
            ++immediate;
    }

    // QUOTED, so a schema that silently shrank is visible: the loop above is vacuously green over an empty
    // parameter list, which is how a census stops counting anything without going red.
    std::printf( "[CloudMaterialSchema] %u of %u parameters declare Rebake; %u declare Immediate\n", rebake,
                 static_cast<uint32_t>( Schema().Params.size() ), immediate );
    EXPECT_EQ( rebake, 20u );
    // FIFTEEN SINCE THE MEDIUM SLOT, which is the fourteen march parameters plus the authored medium
    // itself. It is Immediate and CloudMaterialTiming MEASURES that it is: a medium is GPU code compiled
    // into the march, so no amount of authoring it can move an input of a bake that has already run.
    EXPECT_EQ( immediate, 15u );
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════
// THE SECOND SCHEMA — the authored medium's own properties, resolved out of the SAME `.demat`
// ═════════════════════════════════════════════════════════════════════════════════════════════════════
//
// О1-G-2. A cloud material now answers to two schemas: the shipped one above, mirrored field for field
// onto CloudMaterialValues, and whatever Volume graph its Medium slot names, whose property names were
// invented by the artist who drew it. Both are resolved out of one flat name -> value map.
//
// The load-bearing property is that the two CANNOT be read as each other, and the assertions below are
// that relation rather than a check of either resolver on its own — which is the shape this project has
// paid for repeatedly: both ends correct, the link between them dropping something.

namespace
{
    ShaderParam MediumValue( const char* name, const glm::vec4& defaultValue )
    {
        ShaderParam p;
        p.Name    = name;
        p.Type    = ::Desert::Core::Formats::ShaderValueType::Float;
        p.Default = defaultValue;
        p.Timing  = ::Desert::Core::Formats::ShaderParamTiming::Immediate;
        return p;
    }

    ShaderParam MediumImage( const char* name )
    {
        ShaderParam p;
        p.Name      = name;
        p.IsTexture = true;
        p.Timing    = ::Desert::Core::Formats::ShaderParamTiming::Immediate;
        return p;
    }
} // namespace

TEST( CloudMediumValues, TheSchemaSuppliesDefaultsAndTheMaterialOverwritesByPrefixedName )
{
    const std::vector<ShaderParam> schema = { MediumValue( "Warmth", { 0.25f, 0, 0, 0 } ),
                                              MediumValue( "Bite", { 0.5f, 0, 0, 0 } ), MediumImage( "Streaks" ) };

    Desert::Graphic::MaterialOverrides overrides;
    overrides.Params.push_back( { Desert::Core::CloudMediumOverrideKey( "Bite" ), { 0.75f, 0, 0, 0 } } );
    overrides.Textures.push_back( { Desert::Core::CloudMediumOverrideKey( "Streaks" ), 4242ull } );

    const auto values = Desert::Graphic::BuildCloudMediumValues( schema, overrides );

    ASSERT_EQ( values.Params.size(), 2u );
    EXPECT_FLOAT_EQ( values.Params[0].x, 0.25f ) << "an unauthored property must keep the value the graph "
                                                    "author typed into the node";
    EXPECT_FLOAT_EQ( values.Params[1].x, 0.75f );
    ASSERT_EQ( values.Textures.size(), 1u );
    EXPECT_EQ( static_cast<uint64_t>( values.Textures[0] ), 4242ull );
}

TEST( CloudMediumValues, TheOrderOfTheSchemaISTheLayout )
{
    // The packed vec4s are field 0..n of the medium's std430 block and the handles are
    // Core::kCloudMediumTextureFirst + i. Nothing on either side counts or sorts, so a resolver that
    // dropped a property or reordered one would rebind every slot after it with a perfectly valid
    // descriptor set and no diagnostic anywhere.
    const std::vector<ShaderParam> schema = { MediumValue( "A", { 1, 0, 0, 0 } ), MediumImage( "X" ),
                                              MediumValue( "B", { 2, 0, 0, 0 } ), MediumImage( "Y" ),
                                              MediumValue( "C", { 3, 0, 0, 0 } ) };

    Desert::Graphic::MaterialOverrides overrides;
    overrides.Textures.push_back( { Desert::Core::CloudMediumOverrideKey( "Y" ), 9ull } );

    const auto values = Desert::Graphic::BuildCloudMediumValues( schema, overrides );

    ASSERT_EQ( values.Params.size(), 3u );
    EXPECT_FLOAT_EQ( values.Params[0].x, 1.0f );
    EXPECT_FLOAT_EQ( values.Params[1].x, 2.0f );
    EXPECT_FLOAT_EQ( values.Params[2].x, 3.0f );

    ASSERT_EQ( values.Textures.size(), 2u );
    EXPECT_EQ( static_cast<uint64_t>( values.Textures[0] ), 0ull )
         << "an unassigned image slot is still a slot: dropping it would shift every one after it, and the "
            "four consumers bind by index";
    EXPECT_EQ( static_cast<uint64_t>( values.Textures[1] ), 9ull );
}

TEST( CloudMediumValues, NeitherResolverCanEverSeeTheOtherSchemasKeys )
{
    // THE RELATION, IN BOTH DIRECTIONS, OVER THE REAL SHIPPED SCHEMA.
    //
    // Left to right: a medium key handed to the shipped resolver must change nothing — including when the
    // medium's property is spelled exactly like a shipped one, which is the case that would otherwise
    // retune the layer's CPU bake from a graph node.
    //
    // Right to left: a shipped key handed to the medium resolver must change nothing.
    Desert::Graphic::MaterialOverrides mediumOnly;
    for ( const ShaderParam& p : Schema().Params )
        if ( !p.IsAssetRef() && !p.IsTexture )
            mediumOnly.Params.push_back(
                 { Desert::Core::CloudMediumOverrideKey( p.Name ), { 12345.0f, 0, 0, 0 } } );
    ASSERT_FALSE( mediumOnly.Params.empty() );

    // SCANNED FOR THE SENTINEL RATHER THAN COMPARED, and that is not fussiness: CloudMaterialValues is a
    // struct with padding, and a memcmp of two of them would compare bytes no member owns. A float that
    // exists nowhere in the schema's own defaults is looked for at every offset instead, which is
    // padding-immune and says exactly what it means.
    const CloudMaterialValues attacked = BuildCloudMaterialValues( &Schema(), mediumOnly );
    const auto*               bytes    = reinterpret_cast<const unsigned char*>( &attacked );
    for ( std::size_t at = 0; at + sizeof( float ) <= sizeof( attacked ); ++at )
    {
        float probe = 0.0f;
        std::memcpy( &probe, bytes + at, sizeof( probe ) );
        EXPECT_NE( probe, 12345.0f )
             << "a medium-keyed override reached Graphic::CloudMaterialValues at byte offset " << at
             << ". Both schemas share one map in the `.demat`, and the prefix is the only thing keeping "
                "them apart.";
    }

    const std::vector<ShaderParam>     mediumSchema = { MediumValue( "Coverage", { 0.5f, 0, 0, 0 } ) };
    Desert::Graphic::MaterialOverrides shippedOnly;
    shippedOnly.Params.push_back( { "Coverage", { 0.99f, 0, 0, 0 } } );
    const auto values = Desert::Graphic::BuildCloudMediumValues( mediumSchema, shippedOnly );
    ASSERT_EQ( values.Params.size(), 1u );
    EXPECT_FLOAT_EQ( values.Params[0].x, 0.5f )
         << "a SHIPPED property reached the medium's own block, which would make the artist's node follow "
            "a slider that is not its own.";
}

TEST( CloudMediumValues, TheFingerprintIsZeroExactlyWhenTheMediumContributesNothing )
{
    // The environment bake is rebuilt on this number and costs three quarters of a second. Zero has to
    // mean "nothing to see" and nothing else, or a medium whose values happen to hash to zero would stop
    // the world's light following the sky — the exact shape the variant hash was added to close, one link
    // further along.
    EXPECT_EQ( Desert::Graphic::CloudMediumValuesFingerprint( {} ), 0ull );

    Desert::Graphic::CloudMediumValues a;
    a.Params.push_back( { 1.0f, 0.0f, 0.0f, 0.0f } );
    Desert::Graphic::CloudMediumValues b;
    b.Params.push_back( { 1.0f, 0.0f, 0.0f, 1e-6f } );

    EXPECT_NE( Desert::Graphic::CloudMediumValuesFingerprint( a ), 0ull );
    EXPECT_NE( Desert::Graphic::CloudMediumValuesFingerprint( a ),
               Desert::Graphic::CloudMediumValuesFingerprint( b ) );

    // An IMAGE moves it too. It is the half a byte-hash of the values alone would miss, and a swapped
    // texture is as much a different sky as a swapped number.
    Desert::Graphic::CloudMediumValues withImage = a;
    withImage.Textures.push_back( Desert::Assets::AssetHandle( 7ull ) );
    EXPECT_NE( Desert::Graphic::CloudMediumValuesFingerprint( a ),
               Desert::Graphic::CloudMediumValuesFingerprint( withImage ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
