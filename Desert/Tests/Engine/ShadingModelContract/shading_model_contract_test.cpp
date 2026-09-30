// The shading-model contract (Engine/Core/ShaderCompiler/ShadingModels/,
// ShadingModels/ShadingModelContract.glslh).
//
// Two halves. The shading-word tests pin the TABLE — where the index, the texture count and the two payload floats
// live in GBufferC.w — on both sides of the C++/GLSL boundary; they hold from the contract alone. The registry
// tests pin the RULES — Guid identity, Unlit = 0, at most 16, at most two payload floats, Inputs within
// SurfaceOutput, a Guid-stable index — over the shipped files and over built-up manifests; they need the
// implementation (ShadingModels/*.cpp, compiled in by the premake match).

#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelManifest.hpp>
#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelRegistry.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace SM = Desert::Core::ShadingModels;

    std::filesystem::path ShaderRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" ); ++up )
            here = here.parent_path();
        return here / "Editor" / "Resources" / "Shaders";
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    // `#define DESERT_SHADING_WORD_<NAME> <int>` of the GLSL contract.
    std::map<std::string, int> ShadingWordDefines()
    {
        std::map<std::string, int> defines;
        std::istringstream         in( ReadFile( ShaderRoot() / SM::kContractInclude ) );
        const std::string          prefix = "#define DESERT_SHADING_WORD_";
        for ( std::string line; std::getline( in, line ); )
        {
            if ( !line.starts_with( prefix ) )
                continue;
            std::istringstream fields( line.substr( prefix.size() ) );
            std::string        name;
            int                value = -1;
            fields >> name >> value;
            defines[name] = value;
        }
        return defines;
    }

    const SM::ShadingWordField& Field( std::string_view name )
    {
        const auto* const it = std::ranges::find( SM::kShadingWordFields, name, &SM::ShadingWordField::Name );
        EXPECT_NE( it, SM::kShadingWordFields.end() ) << name;
        return *it;
    }

    SM::ShadingModelManifest Manifest( std::uint64_t guid, const std::string& name, std::vector<std::string> inputs = {} )
    {
        SM::ShadingModelManifest m;
        m.Guid   = Common::UUID( guid );
        m.Name   = name;
        m.Inputs = std::move( inputs );
        m.Body =
             "vec3 Evaluate( DesertLight L, DesertSurface S, DesertPayload P ) { return vec3( 0.0 ); }\n"
             "vec3 EvaluateAmbient( DesertAmbient A, DesertSurface S, DesertPayload P ) { return vec3( 0.0 ); }\n";
        m.SourcePath =
             std::filesystem::path( "ShadingModels" ) / ( name + std::string( SM::kShadingModelExtension ) );
        return m;
    }

    // The two models every registry must have, plus @p extra.
    std::vector<SM::ShadingModelManifest> WithEngineModels( std::vector<SM::ShadingModelManifest> extra )
    {
        extra.push_back( Manifest( SM::kUnlitGuid, "Unlit" ) );
        extra.push_back( Manifest( SM::kDefaultLitGuid, "DefaultLit", { "BaseColor", "Metallic", "Roughness" } ) );
        return extra;
    }

    const std::vector<std::string> kFields{ "BaseColor", "Metallic",    "Roughness",  "Emissive",
                                            "Normal",    "CustomData0", "CustomData1" };
} // namespace

// ---------------------------------------------------------------------------------------------------------------
// The shading word.

TEST( ShadingWord, FieldsAreDisjointAndExactInAFloat )
{
    std::uint32_t used = 0;
    for ( const SM::ShadingWordField& f : SM::kShadingWordFields )
    {
        ASSERT_GT( f.BitCount, 0 ) << f.Name;
        ASSERT_LE( f.FirstBit + f.BitCount, SM::kShadingWordExactBits )
             << f.Name << " reaches past bit " << static_cast<int>( SM::kShadingWordExactBits )
             << ", where a float stops being exact";
        const std::uint32_t mask = ( ( 1u << f.BitCount ) - 1u ) << f.FirstBit;
        EXPECT_EQ( used & mask, 0u ) << f.Name << " overlaps another field of the shading word";
        used |= mask;
    }
}

TEST( ShadingWord, IndexFieldHoldsExactlyTheModelCapacityAndUnlitIsZero )
{
    EXPECT_EQ( std::size_t( 1 ) << Field( "INDEX" ).BitCount, SM::kMaxShadingModels );
    EXPECT_EQ( Field( "INDEX" ).FirstBit, 0 ) << "a word of zero must read as index 0, Unlit";
}

TEST( ShadingWord, PayloadIsTwoFloatsOutsideTheOccupiedFields )
{
    EXPECT_EQ( SM::kMaxPayloadFloats, 2u );
    EXPECT_EQ( SM::kPayloadPins.size(), SM::kMaxPayloadFloats );
    for ( std::size_t i = 0; i < SM::kMaxPayloadFloats; ++i )
    {
        const SM::ShadingWordField& p = Field( std::format( "PAYLOAD{}", i ) );
        for ( const std::string_view occupied : { "INDEX", "TEXTURES" } )
        {
            const SM::ShadingWordField& o = Field( occupied );
            EXPECT_TRUE( p.FirstBit >= o.FirstBit + o.BitCount || o.FirstBit >= p.FirstBit + p.BitCount )
                 << p.Name << " overlaps " << occupied;
        }
    }
}

TEST( ShadingWord, GlslDefinesAreTheCppTable )
{
    const std::map<std::string, int> glsl = ShadingWordDefines();
    ASSERT_FALSE( glsl.empty() ) << "no DESERT_SHADING_WORD_* in " << SM::kContractInclude;

    EXPECT_EQ( glsl.at( "EXACT_BITS" ), SM::kShadingWordExactBits );
    EXPECT_EQ( glsl.at( "INDEX_FIRST_BIT" ), Field( "INDEX" ).FirstBit );
    EXPECT_EQ( glsl.at( "INDEX_BITS" ), Field( "INDEX" ).BitCount );
    EXPECT_EQ( glsl.at( "TEXTURES_FIRST_BIT" ), Field( "TEXTURES" ).FirstBit );
    EXPECT_EQ( glsl.at( "TEXTURES_BITS" ), Field( "TEXTURES" ).BitCount );
    EXPECT_EQ( glsl.at( "PAYLOAD0_FIRST_BIT" ), Field( "PAYLOAD0" ).FirstBit );
    EXPECT_EQ( glsl.at( "PAYLOAD1_FIRST_BIT" ), Field( "PAYLOAD1" ).FirstBit );
    EXPECT_EQ( glsl.at( "PAYLOAD_BITS" ), Field( "PAYLOAD0" ).BitCount );
    EXPECT_EQ( glsl.at( "PAYLOAD_BITS" ), Field( "PAYLOAD1" ).BitCount );
}

// ReceiveSunShadows rides the word's SIGN: outside every magnitude field, and the IEEE sign bit itself, so marking
// it leaves the magnitude the generated unpack reads exact — at 0.0 (an empty Unlit word) and at the largest word.
TEST( ShadingWord, SunShadowReceiveIsTheSignOutsideTheMagnitude )
{
    const SM::ShadingWordField& sign = SM::kShadingWordSignField;
    ASSERT_EQ( sign.BitCount, 1 );
    EXPECT_EQ( sign.FirstBit, 31 ) << "the float's sign bit is bit 31";
    for ( const SM::ShadingWordField& f : SM::kShadingWordFields )
        EXPECT_LE( f.FirstBit + f.BitCount, sign.FirstBit ) << f.Name << " reaches the sign";

    for ( const std::uint32_t magnitude : { 0u, 1u, ( 1u << SM::kShadingWordExactBits ) - 1u } )
    {
        const auto word = static_cast<float>( magnitude );
        const auto marked =
             std::bit_cast<float>( std::bit_cast<std::uint32_t>( word ) | ( 1u << sign.FirstBit ) );
        EXPECT_TRUE( std::signbit( marked ) ) << magnitude;
        EXPECT_FALSE( std::signbit( word ) ) << magnitude;
        EXPECT_EQ( static_cast<std::uint32_t>( std::fabs( marked ) ), magnitude );
    }
    EXPECT_EQ( ShadingWordDefines().at( "RECEIVE_SUN_SHADOWS_BIT" ), sign.FirstBit );
}

// ---------------------------------------------------------------------------------------------------------------
// The shipped models.

TEST( ShippedShadingModels, ScanSucceedsWithUnlitAtZeroAndAtMostSixteen )
{
    const auto registry = SM::ShadingModelRegistry::Scan( ShaderRoot() );
    ASSERT_TRUE( registry.IsSuccess() ) << registry.GetError();

    const auto entries = registry.GetValue().Entries();
    ASSERT_FALSE( entries.empty() );
    ASSERT_LE( entries.size(), SM::kMaxShadingModels );
    EXPECT_EQ( static_cast<std::uint64_t>( entries[0].Manifest.Guid ), SM::kUnlitGuid );
    EXPECT_EQ( entries[0].Manifest.Name, "Unlit" );
    for ( std::size_t i = 0; i < entries.size(); ++i )
        EXPECT_EQ( entries[i].Index, i ) << entries[i].Manifest.Name;
    ASSERT_NE( registry.GetValue().FindByGuid( Common::UUID( SM::kDefaultLitGuid ) ), nullptr );
}

TEST( ShippedShadingModels, EveryInputAndPayloadPinIsASurfaceOutputField )
{
    const auto fields =
         SM::ReadSurfaceOutputFields( ReadFile( ShaderRoot() / "Mesh/Surface/SurfaceTypes.glslh" ) );
    ASSERT_TRUE( fields.IsSuccess() ) << fields.GetError();
    const auto registry = SM::ShadingModelRegistry::Scan( ShaderRoot() );
    ASSERT_TRUE( registry.IsSuccess() ) << registry.GetError();

    const std::vector<std::string>& known = fields.GetValue();
    for ( const std::string_view pin : SM::kPayloadPins )
        EXPECT_NE( std::ranges::find( known, pin ), known.end() ) << pin << " is not a SurfaceOutput field";
    for ( const SM::ShadingModelEntry& e : registry.GetValue().Entries() )
    {
        EXPECT_LE( e.Manifest.Payload.size(), SM::kMaxPayloadFloats ) << e.Manifest.Name;
        for ( const std::string& input : e.Manifest.Inputs )
            EXPECT_NE( std::ranges::find( known, input ), known.end() ) << e.Manifest.Name << ": " << input;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The rules, over built-up manifests.

TEST( ShadingModelRegistryRules, DuplicateGuidIsRefusedNamingBothFiles )
{
    const auto result = SM::ShadingModelRegistry::Build(
         WithEngineModels( { Manifest( 42, "ToonA" ), Manifest( 42, "ToonB" ) } ), kFields );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "ToonA.shadingmodel" ), std::string::npos ) << result.GetError();
    EXPECT_NE( result.GetError().find( "ToonB.shadingmodel" ), std::string::npos ) << result.GetError();
}

TEST( ShadingModelRegistryRules, UnlitIsZeroEvenBelowASmallerGuid )
{
    const auto result = SM::ShadingModelRegistry::Build( WithEngineModels( { Manifest( 1, "Toon" ) } ), kFields );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    EXPECT_EQ( result.GetValue().FindByGuid( Common::UUID( SM::kUnlitGuid ) )->Index, 0 );
    EXPECT_EQ( result.GetValue().FindByName( "Toon" )->Index, 1 )
         << "the others take 1..N in ascending Guid order";
}

TEST( ShadingModelRegistryRules, IndexIsStableUnderFileOrder )
{
    std::vector<SM::ShadingModelManifest> forward = WithEngineModels( { Manifest( 7, "A" ), Manifest( 9, "B" ) } );
    const std::vector<SM::ShadingModelManifest> reversed( forward.rbegin(), forward.rend() );
    const auto                            one = SM::ShadingModelRegistry::Build( forward, kFields );
    const auto                            two = SM::ShadingModelRegistry::Build( reversed, kFields );
    ASSERT_TRUE( one.IsSuccess() && two.IsSuccess() );
    EXPECT_EQ( one.GetValue().IndexLayoutKey(), two.GetValue().IndexLayoutKey() );
}

TEST( ShadingModelRegistryRules, SeventeenModelsAreRefused )
{
    std::vector<SM::ShadingModelManifest> extra;
    for ( std::uint64_t g = 1; g <= SM::kMaxShadingModels - 1; ++g )
        extra.push_back( Manifest( g, std::format( "M{}", g ) ) );
    const auto result = SM::ShadingModelRegistry::Build( WithEngineModels( std::move( extra ) ), kFields );
    EXPECT_FALSE( result.IsSuccess() ) << "17 models cannot fit four bits of index";
}

TEST( ShadingModelRegistryRules, MissingUnlitIsRefused )
{
    const auto result = SM::ShadingModelRegistry::Build(
         { Manifest( SM::kDefaultLitGuid, "DefaultLit" ), Manifest( 5, "Toon" ) }, kFields );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( std::to_string( SM::kUnlitGuid ) ), std::string::npos )
         << result.GetError();
}

TEST( ShadingModelRegistryRules, InputOutsideSurfaceOutputIsRefusedNamingFileAndField )
{
    const auto result =
         SM::ShadingModelRegistry::Build( WithEngineModels( { Manifest( 5, "Cloth", { "Sheen" } ) } ), kFields );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "Cloth.shadingmodel" ), std::string::npos ) << result.GetError();
    EXPECT_NE( result.GetError().find( "Sheen" ), std::string::npos ) << result.GetError();
}

TEST( ShadingModelRegistryRules, MissingInputNamesTheField )
{
    const SM::ShadingModelEntry    toon{ Manifest( 5, "Toon", { "BaseColor", "CustomData0" } ), 1 };
    const std::vector<std::string> written{ "BaseColor" };
    EXPECT_EQ( SM::ShadingModelRegistry::MissingInput( toon, written ),
               std::optional<std::string>( "CustomData0" ) );
    const std::vector<std::string> all{ "BaseColor", "CustomData0" };
    EXPECT_EQ( SM::ShadingModelRegistry::MissingInput( toon, all ), std::nullopt );
}

// ---------------------------------------------------------------------------------------------------------------
// The manifest.

TEST( ShadingModelManifestRules, ThreePayloadFloatsAreRefusedNamingTheFile )
{
    const auto result = SM::ParseShadingModelManifest( "Guid 5;\n"
                                                       "Payload { CustomData0 A; CustomData1 B; CustomData1 C; }\n"
                                                       "Inputs { }\n",
                                                       "ShadingModels/Wide.shadingmodel" );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "Wide.shadingmodel" ), std::string::npos ) << result.GetError();
}

TEST( ShadingModelManifestRules, MissingGuidIsRefused )
{
    const auto result =
         SM::ParseShadingModelManifest( "Payload { }\nInputs { }\n", "ShadingModels/NoGuid.shadingmodel" );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "NoGuid.shadingmodel" ), std::string::npos ) << result.GetError();
}

TEST( ShadingModelManifestRules, NameIsTheFileStemAndBodyFollowsInputs )
{
    const auto result = SM::ParseShadingModelManifest(
         "Guid 5;\nPayload { CustomData0 Bands; }\nInputs { BaseColor; }\nvec3 Evaluate() {}\n",
         "ShadingModels/Toon.shadingmodel" );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    const SM::ShadingModelManifest& m = result.GetValue();
    EXPECT_EQ( m.Name, "Toon" );
    EXPECT_EQ( static_cast<std::uint64_t>( m.Guid ), 5u );
    ASSERT_EQ( m.Payload.size(), 1u );
    EXPECT_EQ( m.Payload[0].Pin, "CustomData0" );
    EXPECT_EQ( m.Payload[0].DisplayName, "Bands" );
    EXPECT_EQ( m.Inputs, std::vector<std::string>{ "BaseColor" } );
    EXPECT_NE( m.Body.find( "vec3 Evaluate()" ), std::string::npos );
}

// THE PASS NORMALIZES, NO MODEL DOES: both passes make their DesertSurface through the contract's one constructor
// (which normalizes N and V), never field by field, and no shipped model re-normalizes S.N — a model that did
// would hide a pass that stopped, and Toon's bands followed the polygons exactly when a pass handed it a short
// normal.
TEST( ShadingModelSurface, EveryPassMakesTheSurfaceThroughTheOneConstructor )
{
    const std::string contract = ReadFile( ShaderRoot() / "ShadingModels" / "ShadingModelContract.glslh" );
    EXPECT_NE( contract.find( "S.N             = normalize( normal );" ), std::string::npos )
         << "DesertMakeSurface no longer normalizes N";
    for ( const auto* pass : { "Mesh/Surface/Pass_Forward.glslh", "Programs/Deferred/DeferredLighting.shader" } )
    {
        const std::string text = ReadFile( ShaderRoot() / pass );
        EXPECT_NE( text.find( "DesertMakeSurface(" ), std::string::npos ) << pass;
        EXPECT_EQ( text.find( "surface.N " ), std::string::npos ) << pass << ": a surface assembled by hand";
    }
    for ( const auto& entry : std::filesystem::directory_iterator( ShaderRoot() / "ShadingModels" ) )
        if ( entry.path().extension() == ".shadingmodel" )
            EXPECT_EQ( ReadFile( entry.path() ).find( "normalize( S.N" ), std::string::npos )
                 << entry.path().filename().string() << " re-normalizes the pass's normal";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
