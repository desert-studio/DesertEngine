// THE CLOUD MEDIUM DOMAIN: what a graph may author, what it may NOT, and the one relation that makes
// the whole mechanism shippable.
//
// ── WHY THE REFUSALS ARE THE INTERESTING HALF ────────────────────────────────────────────────────────
//
// A cloud material carries thirty-five values and twenty of them are inputs to a CPU BAKE — a
// 256x32x256 volume built over several thousand cloud bodies, measured at 3.3 to 14.1 seconds on the
// development machine. The graph in this domain runs on the GPU, per sample, inside a march that reads
// the RESULT of that bake. So a bake input is not merely awkward to reach from a node: it is not in the
// shader's scope at all, and by the time the march runs it has already been consumed.
//
// The teamlead made this the acceptance condition of the architecture, in these terms: the split must be
// VISIBLE to the author in the UI **and** UNEXPRESSIBLE incorrectly — a red test, not a comment and not a
// convention. The visible half is Core::Formats::ShaderParamTiming, which every property of
// CloudRaymarch.shader declares and which the Material Editor draws. This file is the other half.
//
// ── AND IT IS DERIVED, NOT LISTED ────────────────────────────────────────────────────────────────────
//
// There is no hand-written list of parameter names anywhere below. The schema is PARSED out of the
// shipped CloudRaymarch.shader with the engine's own parser, and every assertion is a relation between
// that schema and the graph's own registers:
//
//   * every Rebake property must be REFUSED by the graph compiler, by name;
//   * every readable row must name a property that exists and is Immediate;
//   * every Immediate property must be in one register or the other, so a NEW property cannot be added
//     to the material without somebody deciding whether a graph may read it.
//
// A list would have gone stale on the first property anybody added. This goes red instead.

#include <gtest/gtest.h>

#include "graph_test_tree.hpp"

#include <ShaderGraph.hpp>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphMedium.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace SG = Desert::Editor::ShaderGraph;
using Desert::Core::Preprocess::DShaderParser;
using namespace Desert::Core::Formats;

using Desert::Tests::ShaderGraph::ReadAll;
using Desert::Tests::ShaderGraph::RepoRoot;

namespace
{
    /// The SHIPPED cloud material schema, parsed with the engine's own parser. Everything below is a
    /// relation against this rather than against a copy of it.
    const std::vector<ShaderParam>& CloudSchema()
    {
        static const std::vector<ShaderParam> s_Params = []
        {
            const auto source =
                 ReadAll( RepoRoot() / "Editor/Resources/Shaders/Programs/Clouds/CloudRaymarch.shader" );
            const auto parsed = DShaderParser::Parse( source );
            EXPECT_TRUE( parsed.IsSuccess() )
                 << "the shipped cloud material shader did not parse, so nothing below means anything: "
                 << ( parsed.IsSuccess() ? std::string{} : parsed.GetError() );
            return parsed.IsSuccess() ? parsed.GetValue().Meta.Params : std::vector<ShaderParam>{};
        }();
        return s_Params;
    }

    /// A Volume graph made of exactly the two nodes a new one starts with, and NOTHING wired: the
    /// starter graph the Content Browser creates.
    SG::Document EmptyVolumeDoc()
    {
        SG::Document doc;
        doc.Name   = "TestMedium";
        doc.Domain = static_cast<int>( SG::Domain::Volume );

        auto sample = SG::MakeNode( doc, "CloudSample" );
        auto output = SG::MakeNode( doc, "VolumeOutput" );
        doc.Nodes.push_back( std::move( sample ) );
        doc.Nodes.push_back( std::move( output ) );
        return doc;
    }

    size_t IndexOfInput( const SG::Node& node, const char* name )
    {
        for ( size_t i = 0; i < node.Inputs.size(); ++i )
            if ( node.Inputs[i].Name == name )
                return i;
        return node.Inputs.size();
    }

    SG::Node& NodeOfKind( SG::Document& doc, const char* kind )
    {
        for ( auto& node : doc.Nodes )
            if ( node.Kind == kind )
                return node;
        ADD_FAILURE() << "the document has no '" << kind << "' node";
        return doc.Nodes.front();
    }
} // namespace

// The text a starter Volume graph compiles to, on stdout. Disabled because it asserts nothing; run it
// with
//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpNeutralMedium*
// to obtain a medium that can be dropped on a real cloud material and shot — which is how the
// byte-for-byte half of this mechanism's acceptance is taken, and it needs the EMITTER's own output
// rather than a hand-written copy of it.
TEST( ShaderGraphVolumeDomain, DISABLED_DumpNeutralMedium )
{
    const auto compiled = SG::CompileToDShader( EmptyVolumeDoc() );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

// The same, for a graph that actually WIRES something: the shipped density scaled by a material
// parameter, a warm albedo built from the sample's own depth, and an emission. Its purpose is to be
// handed to shaderc — a text assertion cannot tell whether generated GLSL COMPILES, and the emitter is
// the only thing between an artist's canvas and a shader that does not.
//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpWiredMedium*
TEST( ShaderGraphVolumeDomain, DISABLED_DumpWiredMedium )
{
    SG::Document doc = EmptyVolumeDoc();

    auto           defaults = SG::MakeNode( doc, "DefaultDensity" );
    const uint64_t defOut   = defaults.Outputs[0].Id;
    doc.Nodes.push_back( std::move( defaults ) );

    auto strength         = SG::MakeNode( doc, "CloudParam" );
    strength.ParamName    = "DetailStrength";
    const uint64_t strOut = strength.Outputs[0].Id;
    doc.Nodes.push_back( std::move( strength ) );

    auto           scaled  = SG::MakeNode( doc, "MultiplyFloat" );
    const uint64_t scaledA = scaled.Inputs[0].Id;
    const uint64_t scaledB = scaled.Inputs[1].Id;
    const uint64_t scaledO = scaled.Outputs[0].Id;
    doc.Nodes.push_back( std::move( scaled ) );

    auto warm            = SG::MakeNode( doc, "Vec3Const" );
    warm.Value           = { 1.0f, 0.85f, 0.7f, 0.0f };
    const uint64_t warmO = warm.Outputs[0].Id;
    doc.Nodes.push_back( std::move( warm ) );

    auto           layer  = SG::MakeNode( doc, "LayerAlbedo" );
    const uint64_t layerO = layer.Outputs[0].Id;
    doc.Nodes.push_back( std::move( layer ) );

    auto           tinted  = SG::MakeNode( doc, "MultiplyVec3" );
    const uint64_t tintedA = tinted.Inputs[0].Id;
    const uint64_t tintedB = tinted.Inputs[1].Id;
    const uint64_t tintedO = tinted.Outputs[0].Id;
    doc.Nodes.push_back( std::move( tinted ) );

    auto           sample  = SG::MakeNode( doc, "CloudSample" );
    const uint64_t profile = sample.Outputs[2].Id; // Profile
    doc.Nodes.push_back( std::move( sample ) );

    auto           glow  = SG::MakeNode( doc, "ScaleVec3" );
    const uint64_t glowA = glow.Inputs[0].Id;
    const uint64_t glowB = glow.Inputs[1].Id;
    const uint64_t glowO = glow.Outputs[0].Id;
    doc.Nodes.push_back( std::move( glow ) );

    auto emit            = SG::MakeNode( doc, "Vec3Const" );
    emit.Value           = { 0.2f, 0.05f, 0.0f, 0.0f };
    const uint64_t emitO = emit.Outputs[0].Id;
    doc.Nodes.push_back( std::move( emit ) );

    SG::Node& out = NodeOfKind( doc, "VolumeOutput" );
    doc.Links.push_back( { doc.NextId++, defOut, scaledA } );
    doc.Links.push_back( { doc.NextId++, strOut, scaledB } );
    doc.Links.push_back( { doc.NextId++, scaledO, out.Inputs[IndexOfInput( out, "Density" )].Id } );
    doc.Links.push_back( { doc.NextId++, layerO, tintedA } );
    doc.Links.push_back( { doc.NextId++, warmO, tintedB } );
    doc.Links.push_back( { doc.NextId++, tintedO, out.Inputs[IndexOfInput( out, "Albedo" )].Id } );
    doc.Links.push_back( { doc.NextId++, emitO, glowA } );
    doc.Links.push_back( { doc.NextId++, profile, glowB } );
    doc.Links.push_back( { doc.NextId++, glowO, out.Inputs[IndexOfInput( out, "Emissive" )].Id } );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

// The two media О1-G-2's acceptance is taken with, printed so the frames are shot against the EMITTER's
// own output and not a hand-written imitation of it. Both are NEUTRAL at their declared defaults —
// `Thin` is 1.0 and an unassigned image reads the schema's white — so each must be byte-for-byte the
// baseline until the `.demat` moves one value, which is what makes the positive control a control.
//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpMediumWithAValue*
TEST( ShaderGraphVolumeDomain, DISABLED_DumpMediumWithAValue )
{
    SG::Document doc = EmptyVolumeDoc();
    doc.Name         = "O1G2_Param";

    const uint64_t densityPin =
         NodeOfKind( doc, "VolumeOutput" ).Inputs[IndexOfInput( NodeOfKind( doc, "VolumeOutput" ), "Density" )].Id;

    auto           shipped    = SG::MakeNode( doc, "DefaultDensity" );
    const uint64_t shippedOut = shipped.Outputs[0].Id;
    doc.Nodes.push_back( std::move( shipped ) );

    auto thin              = SG::MakeNode( doc, "FloatParam" );
    thin.ParamName         = "Thin";
    thin.Value             = { 1.0f, 0.0f, 0.0f, 0.0f };
    const uint64_t thinOut = thin.Outputs[0].Id;
    doc.Nodes.push_back( std::move( thin ) );

    auto           mul  = SG::MakeNode( doc, "MultiplyFloat" );
    const uint64_t mulA = mul.Inputs[0].Id;
    const uint64_t mulB = mul.Inputs[1].Id;
    const uint64_t mulO = mul.Outputs[0].Id;
    doc.Nodes.push_back( std::move( mul ) );

    doc.Links.push_back( { doc.NextId++, shippedOut, mulA } );
    doc.Links.push_back( { doc.NextId++, thinOut, mulB } );
    doc.Links.push_back( { doc.NextId++, mulO, densityPin } );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpMediumWithAnImage*
TEST( ShaderGraphVolumeDomain, DISABLED_DumpMediumWithAnImage )
{
    SG::Document doc = EmptyVolumeDoc();
    doc.Name         = "O1G2_Image";

    const uint64_t densityPin =
         NodeOfKind( doc, "VolumeOutput" ).Inputs[IndexOfInput( NodeOfKind( doc, "VolumeOutput" ), "Density" )].Id;

    auto           shipped    = SG::MakeNode( doc, "DefaultDensity" );
    const uint64_t shippedOut = shipped.Outputs[0].Id;
    doc.Nodes.push_back( std::move( shipped ) );

    // The sample position in kilometres, split so its X and Z can be the image's coordinates. A medium
    // has no UV; where an image is read is a decision the graph makes.
    auto           sample   = SG::MakeNode( doc, "CloudSample" );
    const uint64_t position = sample.Outputs[0].Id; // PositionKm
    doc.Nodes.push_back( std::move( sample ) );

    auto           split   = SG::MakeNode( doc, "SplitVec3" );
    const uint64_t splitIn = split.Inputs[0].Id;
    const uint64_t splitX  = split.Outputs[0].Id;
    const uint64_t splitZ  = split.Outputs[2].Id;
    doc.Nodes.push_back( std::move( split ) );

    auto image            = SG::MakeNode( doc, "MediumTexture" );
    image.ParamName       = "Streaks";
    const uint64_t imageU = image.Inputs[0].Id;
    const uint64_t imageV = image.Inputs[1].Id;
    const uint64_t imageR = image.Outputs[1].Id; // the R channel, a Float
    doc.Nodes.push_back( std::move( image ) );

    auto           mul  = SG::MakeNode( doc, "MultiplyFloat" );
    const uint64_t mulA = mul.Inputs[0].Id;
    const uint64_t mulB = mul.Inputs[1].Id;
    const uint64_t mulO = mul.Outputs[0].Id;
    doc.Nodes.push_back( std::move( mul ) );

    doc.Links.push_back( { doc.NextId++, position, splitIn } );
    doc.Links.push_back( { doc.NextId++, splitX, imageU } );
    doc.Links.push_back( { doc.NextId++, splitZ, imageV } );
    doc.Links.push_back( { doc.NextId++, shippedOut, mulA } );
    doc.Links.push_back( { doc.NextId++, imageR, mulB } );
    doc.Links.push_back( { doc.NextId++, mulO, densityPin } );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════
// THE BAKE/MARCH SPLIT — the teamlead's acceptance condition
// ═════════════════════════════════════════════════════════════════════════════════════════════════════

TEST( ShaderGraphVolumeDomain, NoBakeInputIsReachableAsAnInputOfAnyGraphNode )
{
    // THE RED TEST THE CONDITION ASKS FOR. For every property the schema marks Timing(Rebake), a graph
    // that names it must be REFUSED — with the property's own name in the message, because the artist's
    // question is "why can I not use Coverage here" and the answer has to name Coverage.
    int rebakeCount = 0;
    for ( const auto& param : CloudSchema() )
    {
        if ( param.Timing != ShaderParamTiming::Rebake )
            continue;
        ++rebakeCount;

        // It is not in the readable register...
        const bool readable =
             std::any_of( SG::VolumeParams().begin(), SG::VolumeParams().end(),
                          [&param]( const SG::VolumeParam& row ) { return param.Name == row.SchemaName; } );
        EXPECT_FALSE( readable ) << "'" << param.Name
                                 << "' is an input to the CPU bake and the Volume domain offers it as a "
                                    "readable graph value. By the time the march samples anything that "
                                    "bake has already run: the node would read a value that cannot "
                                    "affect the frame it is read in.";

        // ...and a document that names it anyway does not compile.
        SG::Document doc   = EmptyVolumeDoc();
        auto         node  = SG::MakeNode( doc, "CloudParam" );
        node.ParamName     = param.Name;
        const uint64_t out = node.Outputs[0].Id;
        doc.Nodes.push_back( std::move( node ) );

        SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
        const size_t pin    = IndexOfInput( output, "Density" );
        ASSERT_LT( pin, output.Inputs.size() );
        doc.Links.push_back( { doc.NextId++, out, output.Inputs[pin].Id } );

        const auto compiled = SG::CompileToDShader( doc );
        ASSERT_FALSE( compiled.IsSuccess() )
             << "a graph reading the bake input '" << param.Name
             << "' compiled. The split between what the CPU bakes and what the GPU marches would then be "
                "a convention rather than a rule, which is exactly what this suite exists to prevent.";
        EXPECT_NE( compiled.GetError().find( param.Name ), std::string::npos )
             << "the refusal does not name '" << param.Name
             << "', so the artist is told that something is wrong without being told what: "
             << compiled.GetError();
    }

    EXPECT_GT( rebakeCount, 0 ) << "the schema declares no Rebake property at all, so this test asserted "
                                   "nothing. Either the parse failed or the Timing attribute was dropped "
                                   "from CloudRaymarch.shader.";
    std::printf( "[ShaderGraphVolumeDomain] %d bake inputs refused as graph values\n", rebakeCount );
}

TEST( ShaderGraphVolumeDomain, EveryReadableRowNamesAnImmediatePropertyThatExists )
{
    ASSERT_FALSE( CloudSchema().empty() );

    for ( const auto& row : SG::VolumeParams() )
    {
        const auto param = std::find_if( CloudSchema().begin(), CloudSchema().end(),
                                         [&row]( const ShaderParam& p ) { return p.Name == row.SchemaName; } );
        ASSERT_NE( param, CloudSchema().end() )
             << "the Volume domain offers cloud material property '" << row.SchemaName
             << "', which the shipped schema does not declare. The node would emit GLSL naming a field "
                "that does not exist, or — worse — one that means something else.";
        EXPECT_EQ( param->Timing, ShaderParamTiming::Immediate )
             << "'" << row.SchemaName << "' is offered to graphs but is declared "
             << ShaderParamTimingName( param->Timing )
             << ". Only a value the march reads per frame may be read per sample.";
        EXPECT_TRUE( param->AssetKind.empty() ) << "'" << row.SchemaName
                                                << "' is an ASSET REFERENCE, which is a CPU-side handle and not a "
                                                   "number the shader can read at all.";
        EXPECT_NE( std::string( row.Expression ), "" );
        EXPECT_NE( std::string( row.Units ), "" )
             << "'" << row.SchemaName
             << "' has no Units note. DetailTileSize is centimetres in the panel and KILOMETRES in the "
                "medium; a row without that sentence is how the two come to be confused.";
    }
}

TEST( ShaderGraphVolumeDomain, EveryImmediatePropertyIsInOneRegisterOrTheOther )
{
    // THE DIRECTION THAT CATCHES A NEW PROPERTY. Adding one to CloudRaymarch.shader and forgetting the
    // graph entirely is the silent case: the material grows a knob, the graph cannot see it, and nothing
    // anywhere says whether that was intended. Here it is a red test with the property's name in it.
    ASSERT_FALSE( CloudSchema().empty() );

    for ( const auto& param : CloudSchema() )
    {
        if ( param.Timing != ShaderParamTiming::Immediate || !param.AssetKind.empty() )
            continue;

        const bool readable =
             std::any_of( SG::VolumeParams().begin(), SG::VolumeParams().end(),
                          [&param]( const SG::VolumeParam& row ) { return param.Name == row.SchemaName; } );
        const auto excused = std::find_if(
             SG::VolumeParamsOutOfScope().begin(), SG::VolumeParamsOutOfScope().end(),
             [&param]( const SG::VolumeParamOutOfScope& row ) { return param.Name == row.SchemaName; } );
        const bool isExcused = excused != SG::VolumeParamsOutOfScope().end();

        EXPECT_NE( readable, isExcused )
             << "'" << param.Name << "' is "
             << ( readable ? "in BOTH registers, so one of them is lying about it"
                           : "in NEITHER register. It is a per-frame value of the cloud material, so "
                             "somebody has to say whether a medium graph may read it — add it to "
                             "ShaderGraph::VolumeParams(), or to VolumeParamsOutOfScope() with the reason." );

        if ( isExcused )
            EXPECT_FALSE( std::string( excused->Reason ).empty() )
                 << "'" << param.Name
                 << "' is excused with an empty reason, which is the same as no "
                    "register at all: the next reader cannot tell whether the "
                    "omission was decided or forgotten.";
    }
}

TEST( ShaderGraphVolumeDomain, NoAssetSlotOrBakeInputIsEvenNAMEABLEByANode )
{
    // The compiler's refusal above is about a graph that already names one. This is about the palette:
    // a Cloud Material Param node dropped fresh must name something LEGAL, or every new node would be an
    // error the artist has to fix before anything compiles.
    SG::Document doc  = EmptyVolumeDoc();
    const auto   node = SG::MakeNode( doc, "CloudParam" );

    const bool readable =
         std::any_of( SG::VolumeParams().begin(), SG::VolumeParams().end(),
                      [&node]( const SG::VolumeParam& row ) { return node.ParamName == row.SchemaName; } );
    EXPECT_TRUE( readable ) << "a freshly created Cloud Material Param node defaults to '" << node.ParamName
                            << "', which the domain does not expose.";
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════
// THE NEGATIVE CONTROL — a graph that changes nothing
// ═════════════════════════════════════════════════════════════════════════════════════════════════════

TEST( ShaderGraphVolumeDomain, AGraphWithNothingWiredCompilesToTheShippedMediumExactly )
{
    // THE ACCEPTANCE OF THE WHOLE MECHANISM, expressed as text. Every unconnected output pin emits the
    // engine's own default, so the starter graph is the shipped medium under another name — which is
    // what makes "assigning a new graph to a layer changes not one pixel" a thing that can be true.
    // The FRAME half of this claim is measured with DomeSweep; this half is what makes it inspectable.
    const auto compiled = SG::CompileToDShader( EmptyVolumeDoc() );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& text = compiled.GetValue();

    for ( const char* forward :
          { "return CloudDefaultDensity( params, field, positionKm );",
            "return CloudDefaultExtinctionFactor( params, field, positionKm );",
            "return CloudDefaultAlbedo( params, field, positionKm, materialAlbedo );",
            "return CloudDefaultEmissive( params, field, positionKm );",
            "return CloudDefaultOcclusion( params, field, positionKm, ambientOcclusion );" } )
    {
        EXPECT_NE( text.find( forward ), std::string::npos )
             << "an unwired Volume Output pin did not compile to the shipped default. Expected:\n  " << forward
             << "\ngot:\n"
             << text;
    }

    // And the Cloud Sample node, which IS in the document, contributes nothing to a function that does
    // not read it: five separate compilations, not one body with five returns.
    EXPECT_EQ( text.find( "CloudGraphSampleAt" ), std::string::npos )
         << "a node nothing reads was emitted anyway, so every medium would evaluate the whole graph "
            "five times per sample.";
}

TEST( ShaderGraphVolumeDomain, TheEmittedMediumIsAVolumeShaderWithNoStagesOfItsOwn )
{
    const auto compiled = SG::CompileToDShader( EmptyVolumeDoc() );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();

    const auto parsed = DShaderParser::Parse( compiled.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << "the graph emitted text the engine's own parser rejects: "
                                      << parsed.GetError();

    EXPECT_EQ( parsed.GetValue().Meta.Domain, ShaderDomain::Volume );
    EXPECT_TRUE( parsed.GetValue().Meta.IsMediumProgram() )
         << "the emitted shader carries no Medium block, so nothing would ever substitute it.";
    EXPECT_TRUE( parsed.GetValue().Stages.empty() )
         << "a medium is compiled INTO other programs; a stage of its own would be a program nothing "
            "draws.";
    EXPECT_TRUE( parsed.GetValue().Meta.Params.empty() )
         << "the Volume domain exposes no properties of its own — its bindings would have to be free in "
            "all four programs that compile it, and a binding collision between two GLSL declarations is "
            "silent.";
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════
// A GRAPH THAT DOES CHANGE SOMETHING CHANGES IT WHERE IT SAID
// ═════════════════════════════════════════════════════════════════════════════════════════════════════

TEST( ShaderGraphVolumeDomain, AnAuthoredDensityReachesTheDensityFunctionAndNoOther )
{
    SG::Document doc = EmptyVolumeDoc();

    auto half              = SG::MakeNode( doc, "FloatConst" );
    half.Value             = { 0.5f, 0.0f, 0.0f, 0.0f };
    const uint64_t halfOut = half.Outputs[0].Id;
    doc.Nodes.push_back( std::move( half ) );

    auto           scale    = SG::MakeNode( doc, "MultiplyFloat" );
    auto           defaults = SG::MakeNode( doc, "DefaultDensity" );
    const uint64_t scaleA   = scale.Inputs[0].Id;
    const uint64_t scaleB   = scale.Inputs[1].Id;
    const uint64_t scaleOut = scale.Outputs[0].Id;
    const uint64_t defOut   = defaults.Outputs[0].Id;
    doc.Nodes.push_back( std::move( scale ) );
    doc.Nodes.push_back( std::move( defaults ) );

    SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
    const size_t pin    = IndexOfInput( output, "Density" );
    ASSERT_LT( pin, output.Inputs.size() );

    doc.Links.push_back( { doc.NextId++, defOut, scaleA } );
    doc.Links.push_back( { doc.NextId++, halfOut, scaleB } );
    doc.Links.push_back( { doc.NextId++, scaleOut, output.Inputs[pin].Id } );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& text = compiled.GetValue();

    // The density function carries the product...
    EXPECT_NE( text.find( "= CloudDefaultDensity( params, field, positionKm );" ), std::string::npos )
         << "the Default Density node did not emit a call to the shipped chain:\n"
         << text;
    EXPECT_NE( text.find( " * " ), std::string::npos );

    // ...and the four it was not wired into are untouched.
    for ( const char* untouched :
          { "return CloudDefaultExtinctionFactor( params, field, positionKm );",
            "return CloudDefaultAlbedo( params, field, positionKm, materialAlbedo );",
            "return CloudDefaultEmissive( params, field, positionKm );",
            "return CloudDefaultOcclusion( params, field, positionKm, ambientOcclusion );" } )
    {
        EXPECT_NE( text.find( untouched ), std::string::npos )
             << "authoring the DENSITY changed another output too. Expected to still find:\n  " << untouched;
    }
}

TEST( ShaderGraphVolumeDomain, EmissionIsAPinAndReachesTheEmissiveFunction )
{
    // The third output of the accepted contract, and the one that had no home before this domain: a
    // constant emission over a whole layer authors badly and would have cost two more unread slots in
    // the parameter block, so it enters the engine as a pin or not at all (O1_DESIGN §11.5).
    SG::Document doc = EmptyVolumeDoc();

    auto glow              = SG::MakeNode( doc, "Vec3Const" );
    glow.Value             = { 2.0f, 0.5f, 0.1f, 0.0f };
    const uint64_t glowOut = glow.Outputs[0].Id;
    doc.Nodes.push_back( std::move( glow ) );

    SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
    const size_t pin    = IndexOfInput( output, "Emissive" );
    ASSERT_LT( pin, output.Inputs.size() );
    doc.Links.push_back( { doc.NextId++, glowOut, output.Inputs[pin].Id } );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& text = compiled.GetValue();

    EXPECT_NE( text.find( "vec3( 2.0, 0.5, 0.1 )" ), std::string::npos ) << text;
    EXPECT_EQ( text.find( "return CloudDefaultEmissive( params, field, positionKm );" ), std::string::npos )
         << "the Emissive pin was wired and the function still returns the default.";
}

TEST( ShaderGraphVolumeDomain, AnImagesRGBReachesTheAlbedoFunctionAsAVec3 )
{
    // THE PIN THIS TEST EXISTS FOR WAS A DEAD KNOB UNTIL О1-I. `Medium Texture` shipped its whole-sample
    // output as Color (vec4) into a domain whose entire palette is float and vec3, and the canvas links
    // by type EQUALITY — so there was no legal link out of it anywhere, in any graph, ever. Retyped to
    // Vec3 (`RGB`) rather than deleted, because a .dgraph stores pins positionally and dropping output 0
    // would have moved the `R` pin under every saved link.
    //
    // What is asserted here is that the retype made the pin LIVE and not merely differently typed: an
    // image sampled at a coordinate the graph computes reaches CloudSampleAlbedo, as a vec3, and the
    // shipped default for that output is gone. `ShaderGraphCompiler ::
    // NoPinIsOfferedInADomainThatCannotConnectIt` is the other half — it refuses the type, this refuses
    // the silence.
    SG::Document doc = EmptyVolumeDoc();

    auto           sample   = SG::MakeNode( doc, "CloudSample" );
    const uint64_t position = sample.Outputs[0].Id; // PositionKm
    doc.Nodes.push_back( std::move( sample ) );

    auto           split   = SG::MakeNode( doc, "SplitVec3" );
    const uint64_t splitIn = split.Inputs[0].Id;
    const uint64_t splitX  = split.Outputs[0].Id;
    const uint64_t splitZ  = split.Outputs[2].Id;
    doc.Nodes.push_back( std::move( split ) );

    auto image      = SG::MakeNode( doc, "MediumTexture" );
    image.ParamName = "Streaks";
    ASSERT_EQ( image.Outputs.size(), 2u );
    EXPECT_EQ( image.Outputs[0].Name, "RGB" );
    EXPECT_EQ( image.Outputs[0].Type, static_cast<int>( SG::ValueType::Vec3 ) );
    const uint64_t imageU   = image.Inputs[0].Id;
    const uint64_t imageV   = image.Inputs[1].Id;
    const uint64_t imageRGB = image.Outputs[0].Id;
    doc.Nodes.push_back( std::move( image ) );

    SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
    const size_t pin    = IndexOfInput( output, "Albedo" );
    ASSERT_LT( pin, output.Inputs.size() );
    // The link the canvas can now make and could not before: the pin's type and the Albedo input's
    // type are the same value, which is the whole rule.
    ASSERT_EQ( output.Inputs[pin].Type, static_cast<int>( SG::ValueType::Vec3 ) );

    doc.Links.push_back( { doc.NextId++, position, splitIn } );
    doc.Links.push_back( { doc.NextId++, splitX, imageU } );
    doc.Links.push_back( { doc.NextId++, splitZ, imageV } );
    doc.Links.push_back( { doc.NextId++, imageRGB, output.Inputs[pin].Id } );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& text = compiled.GetValue();

    // A vec3 declaration, not a vec4 one: the fourth channel is not silently carried into a domain
    // that has nowhere to put it.
    EXPECT_NE( text.find( "= textureLod( Streaks, vec2( n" ), std::string::npos ) << text;
    EXPECT_NE( text.find( ", 0.0 ).rgb;" ), std::string::npos ) << text;
    EXPECT_EQ( text.find( "vec4 n" ), std::string::npos )
         << "the medium declared a vec4 local, which this domain cannot consume:\n"
         << text;

    // It reaches the ALBEDO function and the shipped default for that one output is replaced, while
    // the other four keep theirs.
    EXPECT_EQ( text.find( "return CloudDefaultAlbedo( params, field, positionKm, materialAlbedo );" ),
               std::string::npos )
         << "the Albedo pin was wired and the function still returns the default:\n"
         << text;
    for ( const char* untouched :
          { "return CloudDefaultDensity( params, field, positionKm );",
            "return CloudDefaultExtinctionFactor( params, field, positionKm );",
            "return CloudDefaultEmissive( params, field, positionKm );",
            "return CloudDefaultOcclusion( params, field, positionKm, ambientOcclusion );" } )
        EXPECT_NE( text.find( untouched ), std::string::npos ) << untouched;

    // And the whole thing is still a shader the ENGINE's own parser reads, carrying the image as a
    // Texture2D property of the medium's own schema.
    auto parsed = DShaderParser::Parse( text );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const auto& params = parsed.GetValue().Meta.Params;
    EXPECT_NE( std::find_if( params.begin(), params.end(),
                             []( const ShaderParam& p ) { return p.Name == "Streaks" && p.IsTexture; } ),
               params.end() )
         << "the image the graph sampled is not a property of the emitted medium";
}

TEST( ShaderGraphVolumeDomain, TheLayersOwnValuesAreRefusedOutsideTheOutputTheyBelongTo )
{
    // Layer Albedo is the `materialAlbedo` ARGUMENT of one of the five functions. Reachable from the
    // Density output it would emit GLSL naming an undeclared variable — a compile error from generated
    // code, on a line the artist never wrote. Refused here instead, naming the node and the pin.
    SG::Document doc = EmptyVolumeDoc();

    auto           layer    = SG::MakeNode( doc, "LayerAlbedo" );
    auto           split    = SG::MakeNode( doc, "SplitVec3" );
    const uint64_t layerOut = layer.Outputs[0].Id;
    const uint64_t splitIn  = split.Inputs[0].Id;
    const uint64_t splitX   = split.Outputs[0].Id;
    doc.Nodes.push_back( std::move( layer ) );
    doc.Nodes.push_back( std::move( split ) );

    SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
    const size_t pin    = IndexOfInput( output, "Density" );
    ASSERT_LT( pin, output.Inputs.size() );
    doc.Links.push_back( { doc.NextId++, layerOut, splitIn } );
    doc.Links.push_back( { doc.NextId++, splitX, output.Inputs[pin].Id } );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_FALSE( compiled.IsSuccess() )
         << "the layer's own albedo was read from the density function, where it does not exist.";
    EXPECT_NE( compiled.GetError().find( "Albedo" ), std::string::npos ) << compiled.GetError();

    // ...and it IS legal where it belongs.
    SG::Document   ok      = EmptyVolumeDoc();
    auto           fine    = SG::MakeNode( ok, "LayerAlbedo" );
    const uint64_t fineOut = fine.Outputs[0].Id;
    ok.Nodes.push_back( std::move( fine ) );
    SG::Node&    okOutput = NodeOfKind( ok, "VolumeOutput" );
    const size_t okPin    = IndexOfInput( okOutput, "Albedo" );
    ASSERT_LT( okPin, okOutput.Inputs.size() );
    ok.Links.push_back( { ok.NextId++, fineOut, okOutput.Inputs[okPin].Id } );

    const auto okCompiled = SG::CompileToDShader( ok );
    EXPECT_TRUE( okCompiled.IsSuccess() ) << okCompiled.GetError();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════
// THE PALETTE
// ═════════════════════════════════════════════════════════════════════════════════════════════════════

TEST( ShaderGraphVolumeDomain, TheDomainOffersNoNodeThatNeedsAVertexStageOrTheSceneColour )
{
    // WHAT IS STILL OUT OF THIS DOMAIN, and the list is shorter than it was. Exposed properties and
    // images are IN as of О1-G-2 — they land in the window Core::kGraphOwnedBindingFirst reserves, which
    // Desert/Tests/Engine/ShaderCacheKey measures free over every shipped pass, and the tests below pin
    // where each of them lands. What remains excluded is what a volume genuinely does not have: a `v_UV`
    // (there is no vertex stage in a compute march), the rendered scene colour, and a wall-clock time
    // that would fight the layer's own wind offset.
    for ( const auto& spec : SG::Specs() )
    {
        if ( !SG::SpecInDomain( spec, SG::Domain::Volume ) )
            continue;
        EXPECT_NE( std::string( spec.Kind ), "TextureSample" );
        EXPECT_NE( std::string( spec.Kind ), "UV" );
        EXPECT_NE( std::string( spec.Kind ), "SceneColor" );
        EXPECT_NE( std::string( spec.Kind ), "Time" );
    }
}

namespace
{
    /// A Volume graph with @p floats Float Params and @p textures Medium Textures, every one of them
    /// WIRED — an unread property is not a property, which is its own test below. The floats and the
    /// texture reads are chained through Multiply (Float) into the Density output, and a Vector 3 Param
    /// feeds Emissive when @p vec3 is asked for.
    SG::Document MediumDocWithProperties( int floats, int textures, bool vec3 = false )
    {
        SG::Document doc = EmptyVolumeDoc();

        // THE PIN IDS ARE TAKEN NOW AND THE NODE REFERENCE IS NOT KEPT: every push_back below can
        // reallocate doc.Nodes, and a reference into it would dangle in a way that happens to work.
        const uint64_t densityPin = NodeOfKind( doc, "VolumeOutput" )
                                         .Inputs[IndexOfInput( NodeOfKind( doc, "VolumeOutput" ), "Density" )]
                                         .Id;
        const uint64_t emissivePin = NodeOfKind( doc, "VolumeOutput" )
                                          .Inputs[IndexOfInput( NodeOfKind( doc, "VolumeOutput" ), "Emissive" )]
                                          .Id;

        uint64_t chain = 0;
        auto     join  = [&]( uint64_t pin )
        {
            if ( chain == 0 )
            {
                chain = pin;
                return;
            }
            auto mul = SG::MakeNode( doc, "MultiplyFloat" );
            doc.Links.push_back( { doc.NextId++, chain, mul.Inputs[0].Id } );
            doc.Links.push_back( { doc.NextId++, pin, mul.Inputs[1].Id } );
            chain = mul.Outputs[0].Id;
            doc.Nodes.push_back( std::move( mul ) );
        };

        for ( int i = 0; i < floats; ++i )
        {
            auto node          = SG::MakeNode( doc, "FloatParam" );
            node.ParamName     = "Amount" + std::to_string( i );
            node.Value         = { 0.25f * static_cast<float>( i + 1 ), 0, 0, 0 };
            const uint64_t pin = node.Outputs[0].Id;
            doc.Nodes.push_back( std::move( node ) );
            join( pin );
        }
        for ( int i = 0; i < textures; ++i )
        {
            auto node          = SG::MakeNode( doc, "MediumTexture" );
            node.ParamName     = "Image" + std::to_string( i );
            const uint64_t pin = node.Outputs[1].Id; // the R pin, a Float
            doc.Nodes.push_back( std::move( node ) );
            join( pin );
        }
        if ( chain != 0 )
            doc.Links.push_back( { doc.NextId++, chain, densityPin } );

        if ( vec3 )
        {
            auto node          = SG::MakeNode( doc, "Vec3Param" );
            node.ParamName     = "Tint";
            node.Value         = { 0.1f, 0.2f, 0.3f, 1.0f };
            const uint64_t pin = node.Outputs[0].Id;
            doc.Nodes.push_back( std::move( node ) );
            doc.Links.push_back( { doc.NextId++, pin, emissivePin } );
        }
        return doc;
    }
} // namespace

TEST( ShaderGraphVolumeDomain, TheMediumsOwnResourcesLandExactlyInTheReservedWindow )
{
    // THE RELATION THIS PINS IS THE ONE THAT CANNOT BE SEEN FROM EITHER SIDE ALONE: the order of the
    // Properties block IS the layout. The runtime packs one vec4 per numeric property in that order and
    // binds the i-th image at Core::kCloudMediumTextureFirst + i, and it never counts anything itself —
    // it reads this schema back. A reordering here that nothing checked would rebind every slot after the
    // first difference, silently, with a perfectly valid descriptor set.
    const int          kTextures = 2;
    const SG::Document doc       = MediumDocWithProperties( /*floats=*/2, kTextures, /*vec3=*/true );
    const auto         compiled  = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();

    const auto parsed = DShaderParser::Parse( compiled.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const auto& meta = parsed.GetValue().Meta;

    ASSERT_EQ( meta.Params.size(), 5u ) << compiled.GetValue();

    std::vector<std::string> values;
    std::vector<std::string> images;
    for ( const ShaderParam& p : meta.Params )
    {
        EXPECT_EQ( p.Timing, ShaderParamTiming::Immediate )
             << p.Name
             << ": a medium is GLSL the march runs per sample, so nothing it declares can be an input to "
                "the CPU bake. An unclassified property is refused by the material window's census.";
        EXPECT_FALSE( p.IsAssetRef() ) << p.Name
                                       << ": the Volume palette has no node for an asset "
                                          "reference, and Graphic::BuildCloudMediumValues skips "
                                          "one — a schema that could carry it would shift the "
                                          "layout of everything after it.";
        ( p.IsTexture ? images : values ).push_back( p.Name );
    }
    EXPECT_EQ( values.size(), 3u );
    EXPECT_EQ( images.size(), static_cast<size_t>( kTextures ) );

    const std::string& text = compiled.GetValue();

    // The block is declared once, at the reserved binding, and its FIELDS are the numeric properties in
    // the schema's own order.
    const std::string bufferDecl =
         "layout( std430, binding = " + std::to_string( Desert::Core::kCloudMediumParamsBinding ) +
         " ) readonly buffer " + Desert::Core::kCloudMediumBlockName;
    EXPECT_NE( text.find( bufferDecl ), std::string::npos ) << text;

    std::size_t cursor = text.find( std::string( "struct " ) + Desert::Core::kCloudMediumStructName );
    ASSERT_NE( cursor, std::string::npos ) << text;
    for ( const std::string& name : values )
    {
        const std::size_t at = text.find( "vec4 " + name + ";", cursor );
        EXPECT_NE( at, std::string::npos ) << name << " is not a field of the medium's block:\n" << text;
        EXPECT_GT( at, cursor ) << name << " is declared out of the schema's order, which IS the layout";
        cursor = at;
    }

    // Every image at its own slot of the window, in the schema's order.
    for ( std::size_t i = 0; i < images.size(); ++i )
    {
        const std::string decl =
             "layout( binding = " + std::to_string( Desert::Core::kCloudMediumTextureFirst + i ) +
             " ) uniform sampler2D " + images[i] + ";";
        EXPECT_NE( text.find( decl ), std::string::npos ) << decl << " is missing from:\n" << text;
    }

    // AND NOTHING BELOW THE WINDOW. A medium declaring anything at an engine binding is the one failure
    // Г17 catches at reflection — in front of the artist who applied the material, which is late.
    for ( uint32_t binding = 0; binding < Desert::Core::kGraphOwnedBindingFirst; ++binding )
    {
        EXPECT_EQ( text.find( "binding = " + std::to_string( binding ) + " " ), std::string::npos )
             << "the emitted medium declares something at engine binding " << binding;
    }
}

TEST( ShaderGraphVolumeDomain, APropertyNoFunctionReadsIsNotDeclaredAtAll )
{
    // TWO RULES IN ONE ASSERTION, and they happen to want the same thing.
    //
    // The contract's: a property that reaches no output is a row in the material window that moves
    // nothing — a dead setting, which §1.3 refuses.
    //
    // The mechanical one, which is the expensive half: a storage block no function reads is eliminated
    // from the SPIR-V, so the renderer — which binds by NUMBER and never consults reflection — would
    // write a descriptor into a layout that does not have that slot.
    SG::Document doc  = EmptyVolumeDoc();
    auto         node = SG::MakeNode( doc, "FloatParam" );
    node.ParamName    = "Unread";
    doc.Nodes.push_back( std::move( node ) );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();

    EXPECT_EQ( compiled.GetValue().find( "Unread" ), std::string::npos ) << compiled.GetValue();
    EXPECT_EQ( compiled.GetValue().find( "Properties" ), std::string::npos ) << compiled.GetValue();
    EXPECT_EQ( compiled.GetValue().find( Desert::Core::kCloudMediumBlockName ), std::string::npos )
         << compiled.GetValue();

    // And it is still byte-for-byte the shipped medium, which is the invariant the whole mechanism rests
    // on: a graph that changes nothing changes nothing.
    const auto neutral = SG::CompileToDShader( EmptyVolumeDoc() );
    ASSERT_TRUE( neutral.IsSuccess() ) << neutral.GetError();
    EXPECT_EQ( compiled.GetValue(), neutral.GetValue() );
}

TEST( ShaderGraphVolumeDomain, AMediumKeyCanNeverBeReadAsAShippedMaterialProperty )
{
    // THE COLLISION THAT WOULD BE SILENT, CLOSED BY CONSTRUCTION. Both schemas are resolved out of ONE
    // flat name -> value map in the `.demat`. A medium property called `Coverage` written under its own
    // name would be picked up by Graphic::ApplyCloudOverride and would retune the layer's CPU bake, with
    // the graph reading it as its own value at the same time — one key, two meanings.
    //
    // The prefix is what makes that unexpressible rather than merely refused: it contains a character no
    // GLSL identifier may contain, so no shipped property name can ever be a medium key and no medium key
    // can ever be a shipped property name. Asserted over the real shipped schema, in both directions.
    ASSERT_FALSE( CloudSchema().empty() );
    for ( const ShaderParam& p : CloudSchema() )
    {
        EXPECT_FALSE( Desert::Core::IsCloudMediumOverrideKey( p.Name ) )
             << p.Name << " is a shipped cloud property whose name reads as a medium key.";
        for ( const ShaderParam& q : CloudSchema() )
            EXPECT_NE( Desert::Core::CloudMediumOverrideKey( q.Name ), p.Name );
    }

    // And the slot that NAMES the medium is not a prefix of its own properties' keys — "Medium" and
    // "Medium." are different keys, which is what lets both live in the same map.
    EXPECT_FALSE( Desert::Core::IsCloudMediumOverrideKey( Desert::Graphic::kCloudMediumSlotName ) );
}

TEST( ShaderGraphVolumeDomain, AMediumPastTheWindowsCapacityIsRefusedByName )
{
    // BOTH CEILINGS ARE REAL RESOURCES AND BOTH ARE NAMED WHEN THEY ARE HIT. The image count is
    // descriptors four shipped programs bind on every frame of every scene, clouds or not; the value
    // count is a buffer allocated once, before any medium exists, because a device allocation inside the
    // frame has nowhere to report a failure.
    {
        const auto compiled = SG::CompileToDShader( MediumDocWithProperties(
             /*floats=*/0, static_cast<int>( Desert::Core::kCloudMediumMaxTextures ) + 1 ) );
        ASSERT_FALSE( compiled.IsSuccess() );
        EXPECT_NE( compiled.GetError().find( "Medium Texture" ), std::string::npos ) << compiled.GetError();
    }
    {
        const auto compiled = SG::CompileToDShader( MediumDocWithProperties(
             static_cast<int>( Desert::Core::kCloudMediumMaxValues ) + 1, /*textures=*/0 ) );
        ASSERT_FALSE( compiled.IsSuccess() );
        EXPECT_NE( compiled.GetError().find( "exposed values" ), std::string::npos ) << compiled.GetError();
    }
    // And exactly at the ceiling it compiles, so the bound is a bound and not an off-by-one.
    EXPECT_TRUE( SG::CompileToDShader(
                      MediumDocWithProperties( static_cast<int>( Desert::Core::kCloudMediumMaxValues ) - 1,
                                               static_cast<int>( Desert::Core::kCloudMediumMaxTextures ),
                                               /*vec3=*/true ) )
                      .IsSuccess() );
}

TEST( ShaderGraphVolumeDomain, EveryVolumeNodeHasACompilerRule )
{
    // The catalogue drives both the palette and the compiler, and the compiler's `else` branch answers a
    // kind it does not know with an error rather than a silent black. Reaching every Volume node once is
    // what turns that branch from a hope into a check.
    for ( const auto& spec : SG::Specs() )
    {
        if ( !SG::SpecInDomain( spec, SG::Domain::Volume ) || std::string( spec.Kind ) == "VolumeOutput" )
            continue;
        if ( spec.Outputs.empty() )
            continue;

        // Each node is wired into the output its type fits, so that a missing rule is the only reason
        // this can fail.
        const char* pin = nullptr;
        switch ( spec.Outputs[0].Type )
        {
            case SG::ValueType::Float:
                pin = std::string( spec.Kind ) == "LayerOcclusion" ? "AmbientOcclusion" : "Density";
                break;
            case SG::ValueType::Vec3:
                pin = std::string( spec.Kind ) == "LayerAlbedo" ? "Albedo" : "Emissive";
                break;
            default:
                continue; // no Volume output pin of that type to wire it into
        }

        SG::Document   doc  = EmptyVolumeDoc();
        auto           node = SG::MakeNode( doc, spec.Kind );
        const uint64_t out  = node.Outputs[0].Id;
        doc.Nodes.push_back( std::move( node ) );

        SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
        const size_t index  = IndexOfInput( output, pin );
        ASSERT_LT( index, output.Inputs.size() ) << spec.Kind;
        doc.Links.push_back( { doc.NextId++, out, output.Inputs[index].Id } );

        const auto compiled = SG::CompileToDShader( doc );
        EXPECT_TRUE( compiled.IsSuccess() )
             << "node '" << spec.Kind
             << "' is in the Cloud Medium palette and does not compile: " << compiled.GetError();
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════
// THE OTHER DIRECTION: WHAT AN AUTHORED MEDIUM CAN SILENCE, IT MUST BE ABLE TO RESTORE
// ═════════════════════════════════════════════════════════════════════════════════════════════════════
//
// Added by О1-D's re-sweep of the authored-control census, from a relation the tree held and nothing
// stated.
//
// A `Medium` graph REPLACES Generated/CloudMedium.glslh. Any value whose only consumer in the whole
// shader tree is Common/CloudMediumDefault.glslh therefore goes inert the moment a graph declines to
// call the shipped body — and if the graph cannot read that value back, an artist has silenced a control
// with no way to honour it. That is DEV_CONTRACT §1.3's dead setting arriving through a door no census
// watched: the knob is wired end to end and does nothing, in exactly one configuration of the material.
//
// It is checked in both halves of what the medium is handed, because the two are restored by different
// means:
//
//   * `params.X` — a MATERIAL value. A graph reads it back with a Cloud Material Param node, so the
//     answer must be a row of ShaderGraph::VolumeParams(), matched on that row's own Expression so
//     there is no second spelling of the member name anywhere.
//   * `field.Y`  — a PRODUCER output. A graph reads it back off the Cloud Sample node, whose pins ARE
//     the members of `CloudGraphSample` (Common/CloudMediumDefault.glslh says so at the struct).
//
// MEASURED WHEN THIS WAS WRITTEN, over the shipped tree: the medium-only material values are
// DetailStrength and DetailTileKm, and both are readable; the medium-only producer outputs are
// DensityScale, DetailFactor, DetailType, ExtinctionFactor and NoiseSlot, and four of the five were pins
// of the Cloud Sample node. The fifth had the register's one row.
//
// THE REGISTER IS NOW EMPTY, AND IT EMPTIED THE ONLY WAY ITS OWN ROW ALLOWED. NoiseSlot's excuse was not
// "this is fine" but "the member alone would be a pin wired nowhere — it is a node plus a member,
// decided together". O1-H added the node (Cloud Noise Volume) and the member in one change, so the row
// was DELETED rather than edited. What survives is the shape: the two directions below still demand that
// every medium-only value be readable, and the third demands that a row still describe the tree — now
// including the direction that catches an excuse outliving its gap.
namespace
{
    /// A value only the shipped medium reads that a graph CANNOT read back, and why that is where it
    /// stands rather than a defect somebody forgot.
    struct MediumOnlyNotReadable
    {
        const char* Member;
        const char* Reason;
    };

    // A `std::array` AND NOT A C ARRAY, because this register was designed to reach zero rows and HAS.
    // A zero-length C array is a GNU extension clang accepts and MSVC refuses outright (C2466), and that
    // exact shape reached `dev` twice in one day from two censuses whose goal was an empty register — so
    // the day the goal was met would have been the day the file stopped compiling on Windows. A type has
    // to be able to express its own structure's success, and this is what that success looks like.
    constexpr std::array<MediumOnlyNotReadable, 0> kMediumOnlyNotReadable = {};

    /// Every member access `receiver.<Ident>` in @p code, comments removed first — prose about a member
    /// is not a read of it, and this file's neighbours discuss these members at length.
    std::set<std::string> MembersRead( const std::string& code, const std::string& receiver )
    {
        std::string stripped;
        stripped.reserve( code.size() );
        for ( std::size_t i = 0; i < code.size(); )
        {
            if ( code.compare( i, 2, "//" ) == 0 )
            {
                while ( i < code.size() && code[i] != '\n' )
                    ++i;
                continue;
            }
            if ( code.compare( i, 2, "/*" ) == 0 )
            {
                i += 2;
                while ( i + 1 < code.size() && code.compare( i, 2, "*/" ) != 0 )
                    ++i;
                i = std::min( i + 2, code.size() );
                continue;
            }
            stripped += code[i++];
        }

        const auto isIdent = []( char c )
        { return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_'; };

        std::set<std::string> members;
        const std::string     needle = receiver + ".";
        for ( std::size_t at = stripped.find( needle ); at != std::string::npos;
              at             = stripped.find( needle, at + 1 ) )
        {
            if ( at != 0 && isIdent( stripped[at - 1] ) )
                continue; // `myParams.` is a different receiver
            std::size_t end = at + needle.size();
            while ( end < stripped.size() && isIdent( stripped[end] ) )
                ++end;
            if ( end > at + needle.size() )
                members.insert( stripped.substr( at + needle.size(), end - at - needle.size() ) );
        }
        return members;
    }

    /// The medium default's members of @p receiver that NO other file in the shader tree reads.
    std::set<std::string> MediumOnlyMembers( const std::string& receiver )
    {
        const std::filesystem::path shaders = RepoRoot() / "Editor/Resources/Shaders";
        const std::filesystem::path medium  = shaders / "Common/CloudMediumDefault.glslh";

        std::set<std::string> mine = MembersRead( ReadAll( medium ), receiver );
        std::set<std::string> others;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( shaders ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const auto extension = entry.path().extension();
            if ( extension != ".glslh" && extension != ".shader" )
                continue;
            if ( std::filesystem::equivalent( entry.path(), medium ) )
                continue;
            for ( const std::string& m : MembersRead( ReadAll( entry.path() ), receiver ) )
                others.insert( m );
        }

        std::set<std::string> only;
        for ( const std::string& m : mine )
            if ( others.count( m ) == 0 )
                only.insert( m );
        return only;
    }

    bool HasRow( const std::string& member )
    {
        return std::any_of( std::begin( kMediumOnlyNotReadable ), std::end( kMediumOnlyNotReadable ),
                            [&member]( const MediumOnlyNotReadable& row ) { return member == row.Member; } );
    }

    /// The body of `struct CloudGraphSample`, straight out of the header a graph is compiled against.
    std::string CloudGraphSampleBody()
    {
        const std::string header =
             ReadAll( RepoRoot() / "Editor/Resources/Shaders/Common/CloudMediumDefault.glslh" );
        const std::size_t open = header.find( "struct CloudGraphSample" );
        if ( open == std::string::npos )
            return {};
        const std::size_t begin = header.find( '{', open );
        const std::size_t end   = header.find( '}', begin );
        if ( begin == std::string::npos || end == std::string::npos )
            return {};
        return header.substr( begin, end - begin );
    }

    /// Can a graph read @p member back — as a pin of the Cloud Sample node, or as a row of the
    /// material-parameter register? One function, because the register's rows and the two directions
    /// below have to agree on what "readable" means or an excuse can outlive its gap by disagreeing.
    bool IsReadableBack( const std::string& member )
    {
        if ( CloudGraphSampleBody().find( " " + member + ";" ) != std::string::npos )
            return true;
        return std::any_of( SG::VolumeParams().begin(), SG::VolumeParams().end(),
                            [&member]( const SG::VolumeParam& row )
                            { return std::string( row.Expression ) == "params." + member; } );
    }
} // namespace

TEST( ShaderGraphVolumeDomain, EveryMaterialValueOnlyTheShippedMediumReadsCanBeReadBackByAGraph )
{
    const std::set<std::string> only = MediumOnlyMembers( "params" );
    ASSERT_FALSE( only.empty() ) << "no material value is read exclusively by the shipped medium, which "
                                    "would mean this scan found nothing rather than that the relation holds";

    for ( const std::string& member : only )
    {
        const bool readable = std::any_of( SG::VolumeParams().begin(), SG::VolumeParams().end(),
                                           [&member]( const SG::VolumeParam& row )
                                           { return std::string( row.Expression ) == "params." + member; } );

        EXPECT_TRUE( readable || HasRow( member ) )
             << "'params." << member
             << "' is read by Common/CloudMediumDefault.glslh and by nothing else in the shader tree, so "
                "an authored Medium graph silences it — and no row of ShaderGraph::VolumeParams() lets a "
                "graph read it back. Either offer it there, or add a row to kMediumOnlyNotReadable saying "
                "why an artist may not restore a control they can switch off.";
    }
}

TEST( ShaderGraphVolumeDomain, EveryProducerOutputOnlyTheShippedMediumReadsIsAPinOfTheCloudSampleNode )
{
    const std::set<std::string> only = MediumOnlyMembers( "field" );
    ASSERT_FALSE( only.empty() ) << "no producer output is read exclusively by the shipped medium, which "
                                    "would mean this scan found nothing";

    // The Cloud Sample node's pins ARE the members of CloudGraphSample, which is why the struct is read
    // out of the header rather than mirrored here: a pin renamed without renaming the member is a GLSL
    // error naming the member, and this assertion is its census.
    const std::string body = CloudGraphSampleBody();
    ASSERT_FALSE( body.empty() ) << "CloudGraphSample is gone, so a graph is handed nothing";

    for ( const std::string& member : only )
    {
        const bool isPin = body.find( " " + member + ";" ) != std::string::npos;

        EXPECT_TRUE( isPin || HasRow( member ) )
             << "'field." << member
             << "' is read by Common/CloudMediumDefault.glslh and by nothing else in the shader tree, so "
                "an authored Medium graph silences it — and it is not a member of CloudGraphSample, so "
                "the Cloud Sample node cannot hand it back. Either add it there together with a node that "
                "can use it, or add a row to kMediumOnlyNotReadable with the reason.";
    }
}

TEST( ShaderGraphVolumeDomain, EveryRowExcusingAnUnreadableValueStillDescribesTheTree )
{
    // A stale exception is how a real gap hides behind an old excuse — the same both-directions
    // discipline kCloudUnreadSlots is held to next door.
    const std::set<std::string> onlyParams = MediumOnlyMembers( "params" );
    const std::set<std::string> onlyFields = MediumOnlyMembers( "field" );

    for ( const MediumOnlyNotReadable& row : kMediumOnlyNotReadable )
    {
        EXPECT_STRNE( row.Reason, "" ) << row.Member << ": a row with an empty reason is not a row";
        EXPECT_TRUE( onlyParams.count( row.Member ) != 0 || onlyFields.count( row.Member ) != 0 )
             << "'" << row.Member
             << "' is excused as a value only the shipped medium reads, and something else in the shader "
                "tree reads it too — or nothing does. Either way the row no longer describes this tree; "
                "delete it.";

        // THE DIRECTION THIS REGISTER DID NOT HAVE, and it is the one that fires on the day the gap is
        // CLOSED. Until O1-H the two checks above passed happily over a row whose member had since become
        // readable: the value was still medium-only, the reason was still non-empty, and nothing anywhere
        // asked whether the excuse was still needed. An excuse that outlives its gap is worse than no
        // register, because the next person reads it as a decision that was taken rather than as a line
        // somebody forgot to delete.
        EXPECT_FALSE( IsReadableBack( row.Member ) )
             << "'" << row.Member
             << "' is excused here as something a graph cannot read back, and a graph CAN read it back "
                "now — it is a pin of the Cloud Sample node or a row of ShaderGraph::VolumeParams(). The "
                "row is spent. DELETE it; do not edit its reason.";
    }

    // Printed and never asserted: a register designed to reach zero is one whose SIZE is the finding, and
    // a number asserted here is a number somebody adjusts instead of closing the gap it stands for.
    std::printf( "[ShaderGraphVolumeDomain] %zu medium-only value(s) a graph still cannot read back\n",
                 kMediumOnlyNotReadable.size() );
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════
// THE NOISE VOLUME, BY SLOT — what closed the register above
// ═════════════════════════════════════════════════════════════════════════════════════════════════════
//
// O1-H. `.dcnv` volumes are a whole population of authored content — the shipped Cirrus names one twice
// as fine as the default — and until this node existed a graph could not touch one: the only number that
// names them lived inside the shipped density chain, and replacing that chain threw the number away with
// it. The node and the CloudGraphSample member ship together, because either alone is a §1.3 refusal in
// one direction or the other: a member with no node is an index wired nowhere, a node with no member can
// only ever be given a constant.
namespace
{
    /// The four channel meanings, in channel order, READ OUT OF THE ENGINE'S OWN enum rather than typed
    /// here. This suite does not link the engine (see its premake5.lua — the graph compiler and the
    /// DShader parser are compiled straight in and nothing else), so the switch is read as text; the
    /// point is the same either way, that a channel renamed at the source turns a pin name red here
    /// instead of leaving a graph reading a frequency it did not ask for.
    std::vector<std::string> NoiseChannelMeanings()
    {
        const std::string source =
             ReadAll( RepoRoot() / "Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp" );
        const std::size_t begin = source.find( "CloudNoiseChannelName(" );
        if ( begin == std::string::npos )
            return {};
        const std::size_t end = source.find( "return \"unknown\"", begin );
        if ( end == std::string::npos )
            return {};

        std::vector<std::string> meanings;
        for ( std::size_t at = source.find( "return \"", begin ); at != std::string::npos && at < end;
              at             = source.find( "return \"", at + 1 ) )
        {
            const std::size_t open  = at + 8;
            const std::size_t close = source.find( '"', open );
            if ( close == std::string::npos )
                break;
            std::string meaning = source.substr( open, close - open );
            std::transform( meaning.begin(), meaning.end(), meaning.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            meanings.push_back( meaning );
        }
        return meanings;
    }

    /// "BillowyCoarse" -> { "billowy", "coarse" }.
    std::vector<std::string> WordsOf( const std::string& camel )
    {
        std::vector<std::string> words;
        for ( char c : camel )
        {
            if ( std::isupper( static_cast<unsigned char>( c ) ) != 0 || words.empty() )
                words.emplace_back();
            words.back() += static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        }
        return words;
    }

    const SG::NodeSpec& SpecOf( const char* kind )
    {
        const SG::NodeSpec* spec = SG::FindSpec( kind );
        EXPECT_NE( spec, nullptr ) << kind << " is not in the catalogue";
        static SG::NodeSpec s_Empty{};
        return spec ? *spec : s_Empty;
    }

    /// A Volume graph whose Density is the shipped density multiplied by one channel of a noise fetch.
    /// @p slotSource picks where the slot comes from: nothing (the node's own default), the Cloud Sample
    /// node's NoiseSlot pin, or a constant.
    enum class SlotSource
    {
        Unwired,
        FromSample,
        Constant
    };

    SG::Document ErodedDifferentlyDoc( SlotSource slotSource, float constantSlot, const char* channel )
    {
        SG::Document doc = EmptyVolumeDoc();

        const uint64_t densityPin = NodeOfKind( doc, "VolumeOutput" )
                                         .Inputs[IndexOfInput( NodeOfKind( doc, "VolumeOutput" ), "Density" )]
                                         .Id;

        auto           shipped    = SG::MakeNode( doc, "DefaultDensity" );
        const uint64_t shippedOut = shipped.Outputs[0].Id;
        doc.Nodes.push_back( std::move( shipped ) );

        auto     noise        = SG::MakeNode( doc, "CloudNoise" );
        uint64_t noiseChannel = 0;
        for ( const auto& pin : noise.Outputs )
            if ( pin.Name == channel )
                noiseChannel = pin.Id;
        EXPECT_NE( noiseChannel, 0u ) << "the Cloud Noise Volume node has no '" << channel << "' output";
        const uint64_t noiseSlotIn = noise.Inputs[0].Id;
        doc.Nodes.push_back( std::move( noise ) );

        if ( slotSource == SlotSource::FromSample )
        {
            SG::Node& sample  = NodeOfKind( doc, "CloudSample" );
            uint64_t  slotOut = 0;
            for ( const auto& pin : sample.Outputs )
                if ( pin.Name == "NoiseSlot" )
                    slotOut = pin.Id;
            EXPECT_NE( slotOut, 0u ) << "the Cloud Sample node has no 'NoiseSlot' output";
            doc.Links.push_back( { doc.NextId++, slotOut, noiseSlotIn } );
        }
        else if ( slotSource == SlotSource::Constant )
        {
            auto value         = SG::MakeNode( doc, "FloatConst" );
            value.Value        = { constantSlot, 0.0f, 0.0f, 0.0f };
            const uint64_t out = value.Outputs[0].Id;
            doc.Nodes.push_back( std::move( value ) );
            doc.Links.push_back( { doc.NextId++, out, noiseSlotIn } );
        }

        auto           mul  = SG::MakeNode( doc, "MultiplyFloat" );
        const uint64_t mulA = mul.Inputs[0].Id;
        const uint64_t mulB = mul.Inputs[1].Id;
        const uint64_t mulO = mul.Outputs[0].Id;
        doc.Nodes.push_back( std::move( mul ) );

        doc.Links.push_back( { doc.NextId++, shippedOut, mulA } );
        doc.Links.push_back( { doc.NextId++, noiseChannel, mulB } );
        doc.Links.push_back( { doc.NextId++, mulO, densityPin } );
        return doc;
    }
} // namespace

TEST( ShaderGraphVolumeDomain, TheCloudNoiseNodesPinsAreTheVolumesOwnFourChannelsInOrder )
{
    // THE PIN INDEX IS THE COMPONENT INDEX — the emitter turns output i into `.xyzw[i]` and consults no
    // table — so the pin NAMES are the only thing that tells an artist which frequency they are wiring.
    // A name that disagrees with the volume is not a compile error anywhere: it is a graph that reads the
    // billowy octave while the canvas says wispy, forever.
    const std::vector<std::string> meanings = NoiseChannelMeanings();
    ASSERT_EQ( meanings.size(), 4u )
         << "Assets::CloudNoiseChannelName no longer names four channels, so this relation read nothing";

    const SG::NodeSpec& spec = SpecOf( "CloudNoise" );
    ASSERT_EQ( spec.Outputs.size(), meanings.size() )
         << "the Cloud Noise Volume node offers " << spec.Outputs.size() << " channel(s) and a `.dcnv` has "
         << meanings.size();

    for ( std::size_t i = 0; i < meanings.size(); ++i )
    {
        EXPECT_EQ( spec.Outputs[i].Type, SG::ValueType::Float )
             << spec.Outputs[i].Name
             << ": a vec4 pin here could be wired nowhere — this domain has no vec4 sink at all.";
        for ( const std::string& word : WordsOf( spec.Outputs[i].Name ) )
            EXPECT_NE( meanings[i].find( word ), std::string::npos )
                 << "pin " << i << " is called '" << spec.Outputs[i].Name << "', and channel " << i
                 << " of a `.dcnv` is '" << meanings[i] << "'. The word '" << word
                 << "' is not in it, so the canvas and the volume disagree "
                    "about which frequency this pin carries.";
    }

    // AND THE PIN'S POSITION IS THE COMPONENT IT READS. The two halves of the naming only mean anything
    // together: a correctly named pin list wired to the wrong component reads the billowy octave from a
    // pin the canvas calls wispy, and nothing in GLSL or in the frame says so out loud.
    for ( std::size_t i = 0; i < meanings.size(); ++i )
    {
        const auto compiled =
             SG::CompileToDShader( ErodedDifferentlyDoc( SlotSource::Unwired, 0.0f, spec.Outputs[i].Name ) );
        ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();

        const std::string component = std::string( 1, "xyzw"[i] );
        EXPECT_NE( compiled.GetValue().find( "* n2." + component + ";" ), std::string::npos )
             << "pin " << i << " ('" << spec.Outputs[i].Name << "') does not read component ." << component
             << " of the fetch:\n"
             << compiled.GetValue();
    }
}

TEST( ShaderGraphVolumeDomain, AnUnwiredCloudNoiseNodeIsExactlyTheShippedFetch )
{
    // THE SAME CONVENTION AN UNWIRED VOLUME OUTPUT PIN FOLLOWS, and it is what makes the node a starting
    // point rather than a form to fill in: the slot is the winning species' own and the coordinate is the
    // one the shipped erosion reads at. An artist reproducing that coordinate by hand would need three
    // frequency constants the palette does not expose and a reciprocal it has no node for.
    const auto compiled = SG::CompileToDShader( ErodedDifferentlyDoc( SlotSource::Unwired, 0.0f, "BillowyFine" ) );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& text = compiled.GetValue();

    EXPECT_NE( text.find( "CLOUD_SAMPLE_NOISE( field.NoiseSlot, "
                          "CloudDefaultNoiseCoordinate( params, positionKm ) )" ),
               std::string::npos )
         << "an unwired Cloud Noise Volume node did not compile to the shipped fetch:\n"
         << text;

    // AND NO CONVERSION AT ALL ON THAT PATH. field.NoiseSlot is already the int the macro wants; putting
    // it through the float pin type and back would be arithmetic on an index nobody asked for.
    EXPECT_EQ( text.find( "CloudNoiseSlotOf" ), std::string::npos ) << text;
}

TEST( ShaderGraphVolumeDomain, AWiredSlotIsRoundedAndClampedBackIntoAnIndex )
{
    const auto compiled =
         SG::CompileToDShader( ErodedDifferentlyDoc( SlotSource::FromSample, 0.0f, "BillowyCoarse" ) );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& text = compiled.GetValue();

    // THE PALETTE HAS ONE NUMERIC PIN TYPE. A slot that travelled through it is a float, and a float that
    // travelled through a Lerp is 0.999998 — truncated that is volume 0, which reads as "my second cloud
    // type stopped eroding" and has no other symptom. The conversion is one named function so the rule
    // has one home.
    const std::size_t at = text.find( "CLOUD_SAMPLE_NOISE( CloudNoiseSlotOf( " );
    ASSERT_NE( at, std::string::npos ) << "a wired slot did not go through the conversion:\n" << text;
    EXPECT_NE( text.find( ".NoiseSlot )", at ), std::string::npos )
         << "the wired slot is not the Cloud Sample node's own:\n"
         << text;
}

TEST( ShaderGraphVolumeDomain, TheCloudNoiseNodeIsLegalInEveryOutputOfTheContract )
{
    // A SCOPE CLAIM, AND SCOPE IS WHAT THIS DOMAIN HAS BEEN WRONG ABOUT TWICE (O1_DESIGN §12.3 against
    // §13.1). Two Volume nodes are legal in exactly one output each because the value they name is an
    // ARGUMENT of one medium function; this one names `params`, `field` and `positionKm`, which all five
    // signatures carry, so it must be legal in all five — asserted rather than assumed, because the day
    // that stops being true the failure is generated GLSL naming an undeclared variable.
    for ( const auto& pin : SpecOf( "VolumeOutput" ).Inputs )
    {
        SG::Document doc = EmptyVolumeDoc();

        auto           noise    = SG::MakeNode( doc, "CloudNoise" );
        const uint64_t noiseOut = noise.Outputs[0].Id;
        doc.Nodes.push_back( std::move( noise ) );

        uint64_t source = noiseOut;
        if ( pin.Type == SG::ValueType::Vec3 )
        {
            // The three-component outputs need a vec3, and the domain builds one by scaling a constant —
            // there is no make-vec3 node and this is how a graph reaches an albedo from a float today.
            auto           white  = SG::MakeNode( doc, "Vec3Const" );
            const uint64_t whiteO = white.Outputs[0].Id;
            doc.Nodes.push_back( std::move( white ) );

            auto           scale  = SG::MakeNode( doc, "ScaleVec3" );
            const uint64_t scaleA = scale.Inputs[0].Id;
            const uint64_t scaleB = scale.Inputs[1].Id;
            source                = scale.Outputs[0].Id;
            doc.Nodes.push_back( std::move( scale ) );

            doc.Links.push_back( { doc.NextId++, whiteO, scaleA } );
            doc.Links.push_back( { doc.NextId++, noiseOut, scaleB } );
        }

        SG::Node& out = NodeOfKind( doc, "VolumeOutput" );
        doc.Links.push_back( { doc.NextId++, source, out.Inputs[IndexOfInput( out, pin.Name )].Id } );

        const auto compiled = SG::CompileToDShader( doc );
        EXPECT_TRUE( compiled.IsSuccess() ) << "the Cloud Noise Volume node is refused in the '" << pin.Name
                                            << "' output: " << compiled.GetError();
    }
}

// The three media O1-H's acceptance is taken with, printed so the frames are shot against the EMITTER's
// own output rather than a hand-written imitation of it. The first is the control that must not move the
// frame at all; the other two differ ONLY in where the slot comes from, which is what makes the pair a
// measurement of the slot rather than of the node.
//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpErodedDifferently*
TEST( ShaderGraphVolumeDomain, DISABLED_DumpErodedDifferentlyFromTheWinnersSlot )
{
    SG::Document doc    = ErodedDifferentlyDoc( SlotSource::FromSample, 0.0f, "BillowyCoarse" );
    doc.Name            = "O1H_SlotFromSample";
    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpErodedDifferentlyFromSlotZero*
TEST( ShaderGraphVolumeDomain, DISABLED_DumpErodedDifferentlyFromSlotZero )
{
    SG::Document doc    = ErodedDifferentlyDoc( SlotSource::Constant, 0.0f, "BillowyCoarse" );
    doc.Name            = "O1H_SlotZero";
    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpErodedDifferentlyFromSlotOne*
TEST( ShaderGraphVolumeDomain, DISABLED_DumpErodedDifferentlyFromSlotOne )
{
    // THE PAIR THAT MEASURES THE SLOT AND NOTHING ELSE. This medium and the one above differ in exactly
    // one literal, so a frame that moves between them moved because the fetch read a DIFFERENT `.dcnv` —
    // which is the whole claim of this task and the one thing the node's existence alone cannot show.
    SG::Document doc    = ErodedDifferentlyDoc( SlotSource::Constant, 1.0f, "BillowyCoarse" );
    doc.Name            = "O1H_SlotOne";
    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════
// SHADOW RAY — the one input that comes from the MARCH and not from the field
// ═════════════════════════════════════════════════════════════════════════════════════════════════════
//
// O1-F's whole subject (O1_DESIGN §3.3, remainder named in §12.7). The medium is compiled into four
// programs and marched by five quadratures, and three of those five sum OPTICAL DEPTH rather than
// radiance — the sun march inside CloudField.glslh, the cloud shadow map, the sky-occlusion volume. Epic
// lets a material cheapen itself there through ShadowSampleDistance; this is the same permission with a
// 0/1 pin, because our shadow marches are binary rather than distance-graded.
//
// AND IT IS A SCOPE QUESTION, not merely a value. The pin is emitted into all five medium functions, but
// only two of them are ever CALLED by a shadow march — a shadow ray never asks for an albedo, an emission
// or an occlusion. In the other three the value is the literal zero, so a graph branching on it there is
// authoring a path that can never be taken: a knob that does nothing, which is what §1.3 of the contract
// forbids in the same breath as a stub. So the compiler refuses it there, and the register saying where
// it is legal is DERIVED below from the shader tree rather than believed.
namespace
{
    // Comments removed. This file's subject is which entry points are CALLED, and every one of the three
    // shadow marches discusses the others in prose right beside the call.
    std::string StripComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        for ( std::size_t i = 0; i < source.size(); )
        {
            if ( source.compare( i, 2, "//" ) == 0 )
            {
                while ( i < source.size() && source[i] != '\n' )
                    ++i;
                continue;
            }
            if ( source.compare( i, 2, "/*" ) == 0 )
            {
                i += 2;
                while ( i + 1 < source.size() && source.compare( i, 2, "*/" ) != 0 )
                    ++i;
                i = std::min( i + 2, source.size() );
                continue;
            }
            out += source[i++];
        }
        return out;
    }

    // The text of the three marches that declare themselves shadow rays. Two of them are whole programs
    // and contribute their whole file; the third is ONE function of a shared header — the sun quadrature
    // runs inside the view march and inside the sky bake, so taking all of CloudField.glslh would sweep in
    // the view march's own calls and make this derivation meaningless.
    std::string ShadowMarchText()
    {
        const std::filesystem::path shaders = RepoRoot() / "Editor/Resources/Shaders";

        const std::string field    = StripComments( ReadAll( shaders / "Common/CloudField.glslh" ) );
        const std::size_t sunMarch = field.find( "float CloudLightOpticalDepth(" );
        EXPECT_NE( sunMarch, std::string::npos ) << "the sun quadrature is not where this derivation looks";
        const std::size_t sunEnd = field.find( "\n}", sunMarch );
        EXPECT_NE( sunEnd, std::string::npos );

        std::string text = field.substr( sunMarch, sunEnd - sunMarch );
        EXPECT_NE( text.find( "ShadowRay = CLOUD_RAY_SHADOW" ), std::string::npos )
             << "the sun quadrature no longer declares itself a shadow ray, so this derivation is reading a "
                "march that is not one";

        text += StripComments( ReadAll( shaders / "Programs/Clouds/CloudShadowMap.shader" ) );
        text += StripComments( ReadAll( shaders / "Programs/Clouds/CloudSkyOcclusionVolume.shader" ) );
        return text;
    }
} // namespace

TEST( ShaderGraphVolumeDomain, ShadowRayIsOfferedInExactlyTheOutputsAShadowMarchAsksFor )
{
    // What the three shadow marches actually call, read out of them.
    const std::string           marches = ShadowMarchText();
    const std::set<std::string> called  = [&marches]
    {
        std::set<std::string> found;
        for ( const char* entryPoint : { "CloudSampleDensity", "CloudSampleExtinctionFactor", "CloudSampleAlbedo",
                                         "CloudSampleEmissive", "CloudSampleOcclusion" } )
            if ( marches.find( std::string( entryPoint ) + "(" ) != std::string::npos )
                found.insert( entryPoint );
        return found;
    }();
    ASSERT_FALSE( called.empty() ) << "no medium entry point was found in the shadow marches at all, which "
                                      "means this test measured nothing";

    std::set<std::string> registered;
    for ( const auto& scope : SG::ShadowRayScopes() )
        registered.insert( scope.EntryPoint );

    EXPECT_EQ( called, registered )
         << "the medium functions a SHADOW march calls and the outputs where the graph offers ShadowRay "
            "have parted company. Both directions are defects: an entry point called by a shadow march and "
            "missing from the register is an output where an author cannot express the saving Epic's "
            "ShadowSampleDistance exists for, and a row in the register that no shadow march reaches is a "
            "pin whose value there is always zero — a branch that can never be taken, offered as a "
            "feature. Fix the register, or fix the march.";

    // And every row names a Volume Output pin that exists, so the two lists cannot drift by a rename.
    const SG::Document doc    = EmptyVolumeDoc();
    const SG::Node*    output = nullptr;
    for ( const auto& node : doc.Nodes )
        if ( node.Kind == "VolumeOutput" )
            output = &node;
    ASSERT_NE( output, nullptr );
    for ( const auto& scope : SG::ShadowRayScopes() )
    {
        bool found = false;
        for ( const auto& pin : output->Inputs )
            found = found || pin.Name == scope.OutputPin;
        EXPECT_TRUE( found ) << "the ShadowRay register names the Volume Output pin '" << scope.OutputPin
                             << "', and the node has no such input";
    }
}

TEST( ShaderGraphVolumeDomain, ShadowRayCompilesIntoTheOutputsItBelongsToAndIsRefusedInTheRest )
{
    std::set<std::string> legal;
    for ( const auto& scope : SG::ShadowRayScopes() )
        legal.insert( scope.OutputPin );

    // Every output of the contract, tried in turn. The two vector pins need a float-to-vec3 step, so the
    // flag reaches them through Scale (Vector 3 x Float) rather than directly — which is also the honest
    // shape of an author's attempt, since tinting by ray kind is exactly what one would try.
    for ( const char* pinName : { "Density", "Extinction", "Albedo", "Emissive", "AmbientOcclusion" } )
    {
        SG::Document doc = EmptyVolumeDoc();

        SG::Node&    sample    = NodeOfKind( doc, "CloudSample" );
        const size_t shadowPin = [&sample]
        {
            for ( size_t i = 0; i < sample.Outputs.size(); ++i )
                if ( sample.Outputs[i].Name == "ShadowRay" )
                    return i;
            return sample.Outputs.size();
        }();
        ASSERT_LT( shadowPin, sample.Outputs.size() ) << "the Cloud Sample node has no ShadowRay output";
        const uint64_t shadowOut = sample.Outputs[shadowPin].Id;

        SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
        const size_t pin    = IndexOfInput( output, pinName );
        ASSERT_LT( pin, output.Inputs.size() );
        const uint64_t target = output.Inputs[pin].Id;
        const bool     isVec3 = std::string( pinName ) == "Albedo" || std::string( pinName ) == "Emissive";

        if ( isVec3 )
        {
            auto white            = SG::MakeNode( doc, "Vec3Const" );
            white.Value           = { 1.0f, 1.0f, 1.0f, 0.0f };
            const uint64_t whiteO = white.Outputs[0].Id;
            doc.Nodes.push_back( std::move( white ) );

            auto           scale  = SG::MakeNode( doc, "ScaleVec3" );
            const uint64_t scaleV = scale.Inputs[0].Id;
            const uint64_t scaleF = scale.Inputs[1].Id;
            const uint64_t scaleO = scale.Outputs[0].Id;
            doc.Nodes.push_back( std::move( scale ) );

            doc.Links.push_back( { doc.NextId++, whiteO, scaleV } );
            doc.Links.push_back( { doc.NextId++, shadowOut, scaleF } );
            doc.Links.push_back( { doc.NextId++, scaleO, target } );
        }
        else
        {
            doc.Links.push_back( { doc.NextId++, shadowOut, target } );
        }

        const auto compiled = SG::CompileToDShader( doc );
        if ( legal.count( pinName ) != 0 )
        {
            ASSERT_TRUE( compiled.IsSuccess() )
                 << "ShadowRay was refused in '" << pinName
                 << "', which IS reached by a shadow march: " << compiled.GetError();
            EXPECT_NE( compiled.GetValue().find( ".ShadowRay" ), std::string::npos )
                 << "'" << pinName << "' compiled without ever reading the flag:\n"
                 << compiled.GetValue();
        }
        else
        {
            ASSERT_FALSE( compiled.IsSuccess() )
                 << "ShadowRay was accepted in '" << pinName
                 << "', where no shadow march ever calls the medium and the value is therefore the "
                    "constant zero. The artist would have authored a branch that can never be taken.";
            EXPECT_NE( compiled.GetError().find( "ShadowRay" ), std::string::npos )
                 << "the refusal does not name the pin the artist wired: " << compiled.GetError();
            EXPECT_NE( compiled.GetError().find( pinName ), std::string::npos )
                 << "the refusal does not name the output it came from: " << compiled.GetError();
        }
    }
}

// O1-J. THE SCOPE REFUSAL ABOVE MATCHES ON A STRING THE DOCUMENT OWNS, and until ValidateGraph compared
// pin NAMES with the catalogue that made it bypassable by editing the file: rename the pin to another
// member of CloudGraphSample of the same type and the `Name == "ShadowRay"` test never fires, while the
// emitter — which builds `<var>.<name>` out of the same stored string — writes a member that exists.
// Valid GLSL, a compiled shader, and a medium reading something the author never wired. The worst of the
// available outcomes, and the one a type check cannot see.
//
// The two halves are asserted together because either alone is misleading: the control shows the scope
// refusal still fires under its own name, so the rename case below is not merely inheriting it.
TEST( ShaderGraphVolumeDomain, RenamingShadowRayToAnotherMemberOfTheSampleStructIsRefusedByName )
{
    const auto docWiringShadowRayInto = []( const char* outputPin, const char* storedName )
    {
        SG::Document doc    = EmptyVolumeDoc();
        SG::Node&    sample = NodeOfKind( doc, "CloudSample" );

        size_t shadowPin = sample.Outputs.size();
        for ( size_t i = 0; i < sample.Outputs.size(); ++i )
            if ( sample.Outputs[i].Name == "ShadowRay" )
                shadowPin = i;
        EXPECT_LT( shadowPin, sample.Outputs.size() ) << "the Cloud Sample node has no ShadowRay output";
        sample.Outputs[shadowPin].Name = storedName;
        const uint64_t shadowOut       = sample.Outputs[shadowPin].Id;

        SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
        const size_t pin    = IndexOfInput( output, outputPin );
        EXPECT_LT( pin, output.Inputs.size() );
        doc.Links.push_back( { doc.NextId++, shadowOut, output.Inputs[pin].Id } );
        return doc;
    };

    // The control: unrenamed, wired into an output no shadow march reaches. Refused, and by the scope
    // rule — the sentence that names the register rather than the catalogue.
    const auto control = SG::CompileToDShader( docWiringShadowRayInto( "AmbientOcclusion", "ShadowRay" ) );
    ASSERT_FALSE( control.IsSuccess() );
    EXPECT_NE( control.GetError().find( "only meaningful" ), std::string::npos )
         << "the scope refusal is not the one that fired: " << control.GetError();

    // And renamed to a member that EXISTS in CloudGraphSample and has the same type, which is the shape
    // that used to compile. `Profile` is a float member of the struct, so the emitted `.Profile` would
    // have been accepted by shaderc without a word.
    const auto renamed = SG::CompileToDShader( docWiringShadowRayInto( "AmbientOcclusion", "Profile" ) );
    ASSERT_FALSE( renamed.IsSuccess() )
         << "a Cloud Sample pin renamed to another member of the same type compiled: the medium now reads "
            "a value nobody wired, and nothing anywhere says so";
    EXPECT_NE( renamed.GetError().find( "Profile" ), std::string::npos )
         << "the refusal does not name the pin as the file stores it: " << renamed.GetError();
    EXPECT_NE( renamed.GetError().find( "ShadowRay" ), std::string::npos )
         << "the refusal does not say which pin the catalogue declares there: " << renamed.GetError();
}

// The other half of the same hole, and the one that reaches shaderc rather than compiling: a name the
// struct does NOT have. It is asserted separately because the two fail in different places without the
// check — this one in the shader compiler, the one above nowhere at all.
TEST( ShaderGraphVolumeDomain, ARenamedCloudSamplePinNeverReachesTheGeneratedGlsl )
{
    // The control first: this exact wiring compiles, so the refusal below is about the rename and not
    // about the graph.
    SG::Document ok       = EmptyVolumeDoc();
    SG::Node&    okSample = NodeOfKind( ok, "CloudSample" );

    size_t okProfile = okSample.Outputs.size();
    for ( size_t i = 0; i < okSample.Outputs.size(); ++i )
        if ( okSample.Outputs[i].Name == "Profile" )
            okProfile = i;
    ASSERT_LT( okProfile, okSample.Outputs.size() );

    SG::Node&    okOutput = NodeOfKind( ok, "VolumeOutput" );
    const size_t density  = IndexOfInput( okOutput, "Density" );
    ASSERT_LT( density, okOutput.Inputs.size() );
    ok.Links.push_back( { ok.NextId++, okSample.Outputs[okProfile].Id, okOutput.Inputs[density].Id } );

    const auto compiled = SG::CompileToDShader( ok );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    EXPECT_NE( compiled.GetValue().find( ".Profile" ), std::string::npos )
         << "the pin's NAME is what the emitter writes as a struct member, which is the premise of this "
            "test; it no longer is:\n"
         << compiled.GetValue();

    // Now the same graph with that pin renamed to something CloudGraphSample does not have. Its type is
    // untouched, so every check ValidateGraph had before this one passes it.
    SG::Document renamed = ok;
    for ( auto& node : renamed.Nodes )
        if ( node.Kind == "CloudSample" )
            node.Outputs[okProfile].Name = "Profil";

    const auto refused = SG::CompileToDShader( renamed );
    ASSERT_FALSE( refused.IsSuccess() )
         << "a renamed pin of the right type compiled, so the emitter has written `.Profil` into a struct "
            "that has no such member — a shaderc error on a line of GENERATED code, which is the failure "
            "ValidateGraph exists to prevent";
    EXPECT_NE( refused.GetError().find( "Profil'" ), std::string::npos )
         << "the refusal does not name the pin: " << refused.GetError();
}

// The two frame controls of O1-F, emitted by the EMITTER rather than hand-written, for the same reason
// the neutral medium is: what has to be proven is that the artist's canvas produces this, not that this
// text works.
//
//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpShadowOnly*
//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpViewOnly*
//
// THEY ARE A PAIR AND ONLY THE PAIR PROVES ANYTHING. `mix( shipped, 0, ShadowRay )` empties the medium on
// shadow rays alone: the clouds keep their exact silhouette and lose their self-shadowing and their shadow
// on the ground. `mix( 0, shipped, ShadowRay )` is the mirror: nothing visible in the sky, and the shadow
// on the ground still there. A flag stuck at either constant would make one of the two into "everything
// changed" and the other into "nothing changed", so the pair separates "the shadow march reads the medium"
// from "the medium changed at all".
namespace
{
    SG::Document RayKindDoc( bool killOnShadowRay )
    {
        SG::Document doc = EmptyVolumeDoc();

        SG::Node& sample    = NodeOfKind( doc, "CloudSample" );
        uint64_t  shadowOut = 0;
        for ( const auto& out : sample.Outputs )
            if ( out.Name == "ShadowRay" )
                shadowOut = out.Id;
        EXPECT_NE( shadowOut, 0u ) << "the Cloud Sample node has no ShadowRay output";

        auto           shipped = SG::MakeNode( doc, "DefaultDensity" );
        const uint64_t defOut  = shipped.Outputs[0].Id;
        doc.Nodes.push_back( std::move( shipped ) );

        auto empty             = SG::MakeNode( doc, "FloatConst" );
        empty.Value            = { 0.0f, 0.0f, 0.0f, 0.0f };
        const uint64_t zeroOut = empty.Outputs[0].Id;
        doc.Nodes.push_back( std::move( empty ) );

        auto           lerp  = SG::MakeNode( doc, "LerpFloat" );
        const uint64_t lerpA = lerp.Inputs[0].Id;
        const uint64_t lerpB = lerp.Inputs[1].Id;
        const uint64_t lerpT = lerp.Inputs[2].Id;
        const uint64_t lerpO = lerp.Outputs[0].Id;
        doc.Nodes.push_back( std::move( lerp ) );

        SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
        const size_t pin    = IndexOfInput( output, "Density" );
        EXPECT_LT( pin, output.Inputs.size() );

        // A is the view ray's answer, B the shadow ray's — mix() reads the flag as its t.
        doc.Links.push_back( { doc.NextId++, killOnShadowRay ? defOut : zeroOut, lerpA } );
        doc.Links.push_back( { doc.NextId++, killOnShadowRay ? zeroOut : defOut, lerpB } );
        doc.Links.push_back( { doc.NextId++, shadowOut, lerpT } );
        doc.Links.push_back( { doc.NextId++, lerpO, output.Inputs[pin].Id } );
        return doc;
    }
} // namespace

TEST( ShaderGraphVolumeDomain, DISABLED_DumpShadowOnlyMedium )
{
    const auto compiled = SG::CompileToDShader( RayKindDoc( /*killOnShadowRay=*/true ) );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

TEST( ShaderGraphVolumeDomain, DISABLED_DumpViewOnlyMedium )
{
    const auto compiled = SG::CompileToDShader( RayKindDoc( /*killOnShadowRay=*/false ) );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}

// The medium O1-F's COST is measured on, and the one an author would actually write: the shipped chain
// for the eye, and the bare profile — no erosion, no noise fetch — for anything that only wants a
// transmittance. It is the exact use Epic documents for ShadowSampleDistance, and unlike the two controls
// above it is meant to look almost the same while costing less.
//   ./ShaderGraphCompiler --gtest_also_run_disabled_tests --gtest_filter=*DumpCheapShadow*
TEST( ShaderGraphVolumeDomain, DISABLED_DumpCheapShadowMedium )
{
    SG::Document doc = EmptyVolumeDoc();

    SG::Node& sample    = NodeOfKind( doc, "CloudSample" );
    uint64_t  shadowOut = 0;
    uint64_t  profile   = 0;
    for ( const auto& out : sample.Outputs )
    {
        if ( out.Name == "ShadowRay" )
            shadowOut = out.Id;
        if ( out.Name == "Profile" )
            profile = out.Id;
    }
    ASSERT_NE( shadowOut, 0u );
    ASSERT_NE( profile, 0u );

    auto           shipped = SG::MakeNode( doc, "DefaultDensity" );
    const uint64_t defOut  = shipped.Outputs[0].Id;
    doc.Nodes.push_back( std::move( shipped ) );

    auto           lerp  = SG::MakeNode( doc, "LerpFloat" );
    const uint64_t lerpA = lerp.Inputs[0].Id;
    const uint64_t lerpB = lerp.Inputs[1].Id;
    const uint64_t lerpT = lerp.Inputs[2].Id;
    const uint64_t lerpO = lerp.Outputs[0].Id;
    doc.Nodes.push_back( std::move( lerp ) );

    SG::Node&    output = NodeOfKind( doc, "VolumeOutput" );
    const size_t pin    = IndexOfInput( output, "Density" );
    ASSERT_LT( pin, output.Inputs.size() );

    doc.Links.push_back( { doc.NextId++, defOut, lerpA } );
    doc.Links.push_back( { doc.NextId++, profile, lerpB } );
    doc.Links.push_back( { doc.NextId++, shadowOut, lerpT } );
    doc.Links.push_back( { doc.NextId++, lerpO, output.Inputs[pin].Id } );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    std::printf( "%s", compiled.GetValue().c_str() );
}
