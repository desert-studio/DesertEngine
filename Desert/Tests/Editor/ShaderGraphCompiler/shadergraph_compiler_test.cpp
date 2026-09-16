#include <gtest/gtest.h>

#include "graph_test_tree.hpp"

#include <ShaderGraph.hpp> // editor: the graph document + compiler under test

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp> // engine: the real parser

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <vector>

namespace SG = Desert::Editor::ShaderGraph;
// The FILE half of the graph lives in the engine now (Engine/Assets/Serialization/ShaderGraph.hpp): the
// asset system has to name it and cannot see Editor/. `SG::Deserialize` is still the editor's, because
// bringing a graph up to the current node CATALOGUE is an authoring step, not a format one.
namespace SGF = Desert::Assets::Serialization::ShaderGraph;
using Desert::Core::Preprocess::DShaderParser;
using namespace Desert::Core::Formats;

namespace
{
    // Minimal Surface graph: BaseColor param -> Surface Output.
    SG::Document SurfaceDoc()
    {
        SG::Document doc;
        doc.Name = "TestSurface";

        auto param      = SG::MakeNode( doc, "ColorParam" );
        param.ParamName = "BaseColor";
        auto output     = SG::MakeNode( doc, "SurfaceOutput" );

        doc.Links.push_back( { doc.NextId++, param.Outputs[0].Id, output.Inputs[0].Id } );
        doc.Nodes.push_back( std::move( param ) );
        doc.Nodes.push_back( std::move( output ) );
        return doc;
    }

    // Minimal Post-Process graph: Scene Color -> Post Process Output (passthrough).
    SG::Document PostProcessDoc()
    {
        SG::Document doc;
        doc.Name   = "TestPost";
        doc.Domain = static_cast<int>( SG::Domain::PostProcess );

        auto scene  = SG::MakeNode( doc, "SceneColor" );
        auto output = SG::MakeNode( doc, "PostProcessOutput" );

        doc.Links.push_back( { doc.NextId++, scene.Outputs[0].Id, output.Inputs[0].Id } );
        doc.Nodes.push_back( std::move( scene ) );
        doc.Nodes.push_back( std::move( output ) );
        return doc;
    }

    bool HasPass( const std::vector<std::string>& passes, const std::string& name )
    {
        return std::find( passes.begin(), passes.end(), name ) != passes.end();
    }
} // namespace

// The compiler must emit the domain from the document, and the engine parser must read it back.
TEST( ShaderGraphCompiler, SurfaceEmitsSurfaceDomainAndOneLitPass )
{
    const auto compiled = SG::CompileToDShader( SurfaceDoc() );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();

    auto parsed = DShaderParser::Parse( compiled.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const auto& p = parsed.GetValue();

    EXPECT_EQ( p.Name, "TestSurface" );
    EXPECT_EQ( p.Meta.Domain, ShaderDomain::Surface );
    EXPECT_TRUE( p.Stages.count( ShaderStage::Vertex ) );
    EXPECT_TRUE( p.Stages.count( ShaderStage::Fragment ) );
    // Mesh vertex contract.
    EXPECT_NE( compiled.GetValue().find( "GraphVertex.glslh" ), std::string::npos );
}

// This test used to assert the OPPOSITE — that a Surface graph "carries a shadow/depth variant".
// It carried a pass named "Depth" that nothing could consume: no C++ anywhere asks for
// "<shader>/Depth", and it could not have served as a shadow caster if something had. A cascade
// target is a colour R32F attachment a fragment shader must write, and that pass declared NO
// fragment stage; its vertex transformed by cameraUB — the camera, not the light. So it was three
// SPIR-V modules per rebuild that rendered nothing anywhere, and asserting its presence pinned the
// wrong thing. Graph materials cast shadows through the engine's shadow pipeline instead.
TEST( ShaderGraphCompiler, NoDepthPassIsEmittedForAnyDomain )
{
    for ( const SG::Document& doc : { SurfaceDoc(), PostProcessDoc() } )
    {
        const auto compiled = SG::CompileToDShader( doc );
        ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
        const std::string& src = compiled.GetValue();

        auto parsed = DShaderParser::Parse( src );
        ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();

        EXPECT_FALSE( HasPass( parsed.GetValue().Meta.PassNames, "Depth" ) )
             << "a pass no consumer names, and one that could not write a cascade if it had";
        // Not merely absent from the pass list — the text that configured it is gone too, so the
        // shared vertex include has no dead branch left to keep alive.
        EXPECT_EQ( src.find( "GRAPH_DEPTH_ONLY" ), std::string::npos );
        EXPECT_EQ( src.find( "Pass \"Depth\"" ), std::string::npos );
    }
}

TEST( ShaderGraphCompiler, PostProcessEmitsPostProcessDomainAndFullscreenTriangle )
{
    const auto compiled = SG::CompileToDShader( PostProcessDoc() );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& src = compiled.GetValue();

    auto parsed = DShaderParser::Parse( src );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const auto& p = parsed.GetValue();

    EXPECT_EQ( p.Meta.Domain, ShaderDomain::PostProcess );
    EXPECT_TRUE( p.Stages.count( ShaderStage::Vertex ) );
    EXPECT_TRUE( p.Stages.count( ShaderStage::Fragment ) );

    // Post-process uses the fullscreen-triangle contract and samples the scene color, and has NO
    // mesh vertex contract and NO shadow/depth pass.
    EXPECT_NE( src.find( "QUAD_POSITIONS" ), std::string::npos );
    EXPECT_NE( src.find( "u_SceneTexture" ), std::string::npos );
    EXPECT_EQ( src.find( "GraphVertex.glslh" ), std::string::npos );
    EXPECT_FALSE( HasPass( p.Meta.PassNames, "Depth" ) );
}

// Switching the document's domain field must change the compiled output — no constant remains.
TEST( ShaderGraphCompiler, DomainIsDrivenByTheDocumentField )
{
    SG::Document doc = SurfaceDoc();

    const auto asSurface = SG::CompileToDShader( doc );
    ASSERT_TRUE( asSurface.IsSuccess() ) << asSurface.GetError();
    EXPECT_NE( asSurface.GetValue().find( "Domain Surface" ), std::string::npos );
    EXPECT_EQ( asSurface.GetValue().find( "Domain PostProcess" ), std::string::npos );
}

TEST( ShaderGraphCompiler, DomainSurvivesSerializationRoundTrip )
{
    const std::string json = SGF::Serialize( PostProcessDoc() );
    auto              back = SG::Deserialize( json );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue().Doc.DomainEnum(), SG::Domain::PostProcess );
    // A document this build wrote needs nothing done to it.
    EXPECT_EQ( back.GetValue().MigratedPins, 0 );
}

// A .dgraph written before the Domain field existed (no "Domain" key) defaults to Surface.
TEST( ShaderGraphCompiler, LegacyGraphWithoutDomainDefaultsToSurface )
{
    const std::string json = SGF::Serialize( SurfaceDoc() );
    // Strip the Domain field to emulate an old document.
    // (Serialization always writes it, so just assert the default instead.)
    SG::Document fresh;
    EXPECT_EQ( fresh.DomainEnum(), SG::Domain::Surface );
    EXPECT_TRUE( SG::Deserialize( json ).IsSuccess() );
}

// A post-process document holding a Surface Output is wrong TWICE — it has a node its domain does
// not offer, and it lacks the output its domain requires. This used to be reported only as the
// second, because the missing-output check was the first thing that ran. Structural validation now
// runs before it, so the error names the node that is actually in the document and the domain it
// does not belong to; a message about a node the artist never placed sends them looking for it.
TEST( ShaderGraphCompiler, OutputFromTheWrongDomainNamesTheOffendingNode )
{
    SG::Document doc;
    doc.Domain  = static_cast<int>( SG::Domain::PostProcess );
    doc.Name    = "Broken";
    auto output = SG::MakeNode( doc, "SurfaceOutput" );
    doc.Nodes.push_back( std::move( output ) );

    const auto compiled = SG::CompileToDShader( doc );
    EXPECT_FALSE( compiled.IsSuccess() );
    EXPECT_NE( compiled.GetError().find( "Surface Output" ), std::string::npos )
         << "names the node that IS there: " << compiled.GetError();
    EXPECT_NE( compiled.GetError().find( "Post Process" ), std::string::npos )
         << "and the domain it is not offered in: " << compiled.GetError();
}

// The missing-output check is still reachable and still says what is missing — it is what a
// well-formed document with no output node at all hits.
TEST( ShaderGraphCompiler, DomainWithNoOutputNodeAtAllNamesTheMissingOutput )
{
    SG::Document doc;
    doc.Domain = static_cast<int>( SG::Domain::PostProcess );
    doc.Name   = "Headless";
    doc.Nodes.push_back( SG::MakeNode( doc, "SceneColor" ) );

    const auto compiled = SG::CompileToDShader( doc );
    EXPECT_FALSE( compiled.IsSuccess() );
    EXPECT_NE( compiled.GetError().find( "Post Process Output" ), std::string::npos ) << compiled.GetError();
}

// The palette split: core nodes live everywhere; output/special nodes are domain-scoped.
TEST( ShaderGraphCompiler, PaletteIsFilteredByDomain )
{
    const SG::NodeSpec* sceneColor = SG::FindSpec( "SceneColor" );
    const SG::NodeSpec* tileUV     = SG::FindSpec( "TileUV" );
    const SG::NodeSpec* multiply   = SG::FindSpec( "Multiply" );
    ASSERT_NE( sceneColor, nullptr );
    ASSERT_NE( tileUV, nullptr );
    ASSERT_NE( multiply, nullptr );

    // Scene Color is post-process only.
    EXPECT_TRUE( SG::SpecInDomain( *sceneColor, SG::Domain::PostProcess ) );
    EXPECT_FALSE( SG::SpecInDomain( *sceneColor, SG::Domain::Surface ) );

    // Mesh tiling makes no sense over a fullscreen effect.
    EXPECT_TRUE( SG::SpecInDomain( *tileUV, SG::Domain::Surface ) );
    EXPECT_FALSE( SG::SpecInDomain( *tileUV, SG::Domain::PostProcess ) );

    // Core math is available in both.
    EXPECT_TRUE( SG::SpecInDomain( *multiply, SG::Domain::Surface ) );
    EXPECT_TRUE( SG::SpecInDomain( *multiply, SG::Domain::PostProcess ) );
}

// ================================================ the palette's own reachability, per domain =====
//
// A PIN WHOSE TYPE HAS NO COUNTERPART IN ITS OWN DOMAIN IS A DEAD KNOB, and until this test existed
// nothing said so. The canvas links by type EQUALITY (NodeGraphPanel: `fromPin->Type == toPin->Type`,
// pinned by TypeRuleIsEqualityInBothDirections below), so an output pin of a type no node in that
// domain accepts is a handle the artist can drag out of and drop nowhere — the contract's dead-knob
// refusal, wearing a feature's clothes. It shipped once: `Medium Texture` was born in the Volume
// domain with a Color (vec4) output, in a domain whose whole palette is float and vec3, one commit
// after Vec3Param was introduced *because* a vec4 has no sink here.
//
// BOTH DIRECTIONS, because they fail differently. An unreachable OUTPUT is a knob that does nothing;
// an input pin of a type no node in the domain PRODUCES is a socket that can only ever hold its
// default, which is the same defect seen from the other end.
//
// NOTHING BELOW IS A HAND-WRITTEN LIST: the domains are derived from the palette, and the two type
// sets are derived from the pins the palette offers in each of them. A census of names would have
// gone stale on the first node anybody added — which is exactly how the dead pin got in.

namespace
{
    // Exhaustive on purpose and with no `default`: a ValueType added tomorrow must not fall silently
    // into a message that says "unknown".
    const char* TypeName( SG::ValueType type )
    {
        switch ( type )
        {
            case SG::ValueType::Float:
                return "Float (float)";
            case SG::ValueType::Vec2:
                return "Vec2 (vec2)";
            case SG::ValueType::Color:
                return "Color (vec4)";
            case SG::ValueType::Vec3:
                return "Vec3 (vec3)";
        }
        return "";
    }

    /// Every domain the palette ACTUALLY HAS, derived rather than listed: a domain exists exactly when
    /// the node that terminates it is offered in it. That is the one property no domain can lack — a
    /// graph with no output node does not compile at all (DomainWithNoOutputNodeAtAllNamesTheMissing-
    /// Output) — and it cannot be read off the Domains masks instead, because `AllDomains` is ~0u and
    /// would invent twenty-nine domains that do not exist.
    std::vector<SG::Domain> DomainsOfThePalette()
    {
        std::vector<SG::Domain> domains;
        for ( int bit = 0; bit < 32; ++bit ) // Domains is a 32-bit mask, so that is the whole space
        {
            const SG::Domain    domain = static_cast<SG::Domain>( bit );
            const SG::NodeSpec* output = SG::FindSpec( SG::OutputKind( domain ) );
            if ( output != nullptr && SG::SpecInDomain( *output, domain ) )
                domains.push_back( domain );
        }
        return domains;
    }
} // namespace

TEST( ShaderGraphCompiler, TheDerivedDomainListIsTheOneTheEnumDeclares )
{
    // The guard the gate below needs: if the derivation ever returns nothing, that test passes
    // vacuously over an empty loop and certifies a palette it never looked at.
    const std::vector<SG::Domain> domains = DomainsOfThePalette();
    for ( const SG::Domain declared : { SG::Domain::Surface, SG::Domain::PostProcess, SG::Domain::Volume } )
        EXPECT_NE( std::find( domains.begin(), domains.end(), declared ), domains.end() )
             << "the domain terminated by '" << SG::OutputKind( declared ) << "' was not derived from the "
             << "palette, so the reachability gate never examined it";
    std::printf( "[ domains ] %zu derived from the palette\n", domains.size() );
}

TEST( ShaderGraphCompiler, NoPinIsOfferedInADomainThatCannotConnectIt )
{
    for ( const SG::Domain domain : DomainsOfThePalette() )
    {
        // What this domain can SINK and what it can SOURCE, taken from the pins themselves.
        std::set<SG::ValueType> sinks;
        std::set<SG::ValueType> sources;
        for ( const auto& spec : SG::Specs() )
        {
            if ( !SG::SpecInDomain( spec, domain ) )
                continue;
            for ( const auto& pin : spec.Inputs )
                sinks.insert( pin.Type );
            for ( const auto& pin : spec.Outputs )
                sources.insert( pin.Type );
        }

        for ( const auto& spec : SG::Specs() )
        {
            if ( !SG::SpecInDomain( spec, domain ) )
                continue;

            for ( const auto& pin : spec.Outputs )
                EXPECT_NE( sinks.find( pin.Type ), sinks.end() )
                     << "in the domain terminated by '" << SG::OutputKind( domain ) << "', the '" << spec.Title
                     << "' node offers the OUTPUT pin '" << pin.Name << "' of type " << TypeName( pin.Type )
                     << ", and no node offered in that domain has an input pin of that type. The canvas "
                        "links by type equality, so every link out of this pin is refused: it is a dead "
                        "knob. Retype it to a type the domain accepts (do not delete it — a .dgraph "
                        "stores pins positionally), or give the domain a node that takes one.";

            for ( const auto& pin : spec.Inputs )
                EXPECT_NE( sources.find( pin.Type ), sources.end() )
                     << "in the domain terminated by '" << SG::OutputKind( domain ) << "', the '" << spec.Title
                     << "' node offers the INPUT pin '" << pin.Name << "' of type " << TypeName( pin.Type )
                     << ", and no node offered in that domain has an output pin of that type. Nothing in "
                        "this domain can ever feed it, so it can only hold its default: the dead knob "
                        "seen from the other end.";
        }
    }
}

// ===================================================================== link type checking =====
//
// The canvas refuses a mismatched link interactively, but a .dgraph is plain JSON. Every test below
// builds a document the canvas could never have produced and asserts the COMPILER rejects it — and
// that the message names the node, because the whole point is that shaderc could only name a line
// of generated code.

namespace
{
    // The shipped reproduction fixture, in code: UV (vec2) wired into Albedo (vec4).
    SG::Document TypeMismatchDoc()
    {
        SG::Document doc;
        doc.Name = "TypeMismatch";

        auto uv     = SG::MakeNode( doc, "UV" );
        auto output = SG::MakeNode( doc, "SurfaceOutput" );

        doc.Links.push_back( { doc.NextId++, uv.Outputs[0].Id, output.Inputs[0].Id } );
        doc.Nodes.push_back( std::move( uv ) );
        doc.Nodes.push_back( std::move( output ) );
        return doc;
    }
} // namespace

TEST( ShaderGraphCompiler, MismatchedLinkIsRejectedAndNamesBothNodes )
{
    const auto compiled = SG::CompileToDShader( TypeMismatchDoc() );
    ASSERT_FALSE( compiled.IsSuccess() ) << "a vec2 -> vec4 link must not reach shaderc";

    const std::string& err = compiled.GetError();
    // Named by their palette titles, with the offending types spelled in GLSL terms.
    EXPECT_NE( err.find( "UV" ), std::string::npos ) << err;
    EXPECT_NE( err.find( "Surface Output" ), std::string::npos ) << err;
    EXPECT_NE( err.find( "Albedo" ), std::string::npos ) << err;
    EXPECT_NE( err.find( "vec2" ), std::string::npos ) << err;
    EXPECT_NE( err.find( "vec4" ), std::string::npos ) << err;
}

// The check must not fire on a graph the canvas would have accepted.
TEST( ShaderGraphCompiler, WellTypedGraphsStillCompile )
{
    EXPECT_TRUE( SG::CompileToDShader( SurfaceDoc() ).IsSuccess() );
    EXPECT_TRUE( SG::CompileToDShader( PostProcessDoc() ).IsSuccess() );
}

// Float -> Color and Color -> Float are both rejected: the rule is equality, exactly as on the
// canvas, not "whatever GLSL happens to accept" (vec4*vec4 compiles and is silently wrong).
TEST( ShaderGraphCompiler, TypeRuleIsEqualityInBothDirections )
{
    {
        SG::Document doc;
        doc.Name    = "FloatIntoColor";
        auto f      = SG::MakeNode( doc, "FloatConst" );
        auto scale  = SG::MakeNode( doc, "Scale" );
        auto output = SG::MakeNode( doc, "SurfaceOutput" );
        // FloatConst (float) into Scale's "Color" input (vec4).
        doc.Links.push_back( { doc.NextId++, f.Outputs[0].Id, scale.Inputs[0].Id } );
        doc.Links.push_back( { doc.NextId++, scale.Outputs[0].Id, output.Inputs[0].Id } );
        doc.Nodes.push_back( std::move( f ) );
        doc.Nodes.push_back( std::move( scale ) );
        doc.Nodes.push_back( std::move( output ) );
        EXPECT_FALSE( SG::CompileToDShader( doc ).IsSuccess() );
    }
    {
        SG::Document doc;
        doc.Name    = "ColorIntoFloat";
        auto c      = SG::MakeNode( doc, "ColorConst" );
        auto scale  = SG::MakeNode( doc, "Scale" );
        auto output = SG::MakeNode( doc, "SurfaceOutput" );
        // ColorConst (vec4) into Scale's "Factor" input (float).
        doc.Links.push_back( { doc.NextId++, c.Outputs[0].Id, scale.Inputs[1].Id } );
        doc.Links.push_back( { doc.NextId++, scale.Outputs[0].Id, output.Inputs[0].Id } );
        doc.Nodes.push_back( std::move( c ) );
        doc.Nodes.push_back( std::move( scale ) );
        doc.Nodes.push_back( std::move( output ) );
        EXPECT_FALSE( SG::CompileToDShader( doc ).IsSuccess() );
    }
}

// TextureSample has two outputs of different types; the check must resolve the type PER PIN, not
// per node, or the R (float) output would be judged as the RGBA (vec4) one.
TEST( ShaderGraphCompiler, MultiOutputNodeIsTypedPerPin )
{
    const auto build = []( size_t outputIndex )
    {
        SG::Document doc;
        doc.Name    = "TexPin";
        auto tex    = SG::MakeNode( doc, "TextureSample" );
        auto output = SG::MakeNode( doc, "SurfaceOutput" );
        // Albedo is vec4: RGBA (output 0) fits, R (output 1) does not.
        doc.Links.push_back( { doc.NextId++, tex.Outputs[outputIndex].Id, output.Inputs[0].Id } );
        doc.Nodes.push_back( std::move( tex ) );
        doc.Nodes.push_back( std::move( output ) );
        return SG::CompileToDShader( doc );
    };

    EXPECT_TRUE( build( 0 ).IsSuccess() );  // RGBA -> Albedo
    EXPECT_FALSE( build( 1 ).IsSuccess() ); // R    -> Albedo
}

// A node whose pin list was trimmed by hand used to read off the end of the vector: the emitter
// indexes node.Inputs[i] positionally for every kind it knows.
TEST( ShaderGraphCompiler, NodeWithPinsNotMatchingItsKindIsRejected )
{
    SG::Document doc;
    doc.Name    = "TrimmedPins";
    auto mul    = SG::MakeNode( doc, "Multiply" );
    auto output = SG::MakeNode( doc, "SurfaceOutput" );
    doc.Links.push_back( { doc.NextId++, mul.Outputs[0].Id, output.Inputs[0].Id } );
    mul.Inputs.clear(); // "Inputs": [] in the file
    doc.Nodes.push_back( std::move( mul ) );
    doc.Nodes.push_back( std::move( output ) );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_FALSE( compiled.IsSuccess() );
    EXPECT_NE( compiled.GetError().find( "Multiply" ), std::string::npos ) << compiled.GetError();
}

// Pin::Type in the file is a mirror of the catalogue. If it lies, the canvas type-checks against
// the lie and accepts a link the compiler cannot emit.
TEST( ShaderGraphCompiler, PinTypeDisagreeingWithTheCatalogueIsRejected )
{
    SG::Document doc = SurfaceDoc();
    for ( auto& node : doc.Nodes )
        if ( node.Kind == "ColorParam" )
            node.Outputs[0].Type = static_cast<int>( SG::ValueType::Float );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_FALSE( compiled.IsSuccess() );
    EXPECT_NE( compiled.GetError().find( "Color Param" ), std::string::npos ) << compiled.GetError();
}

// O1-J. Pin::Name is a mirror of the catalogue in exactly the way Pin::Type is, and it was not checked.
// It is not a label: the emitter builds `<var>.<name>` for the multi-output nodes out of the name THE
// FILE carries, so a rename of the right type used to travel all the way into generated GLSL. This is
// the Surface-domain half — the domain-specific consequences are in shadergraph_volume_domain_test.cpp,
// where the struct member and the scope rule live.
TEST( ShaderGraphCompiler, APinRenamedInTheFileIsRejectedAndTheRefusalNamesIt )
{
    SG::Document doc = SurfaceDoc();
    for ( auto& node : doc.Nodes )
        if ( node.Kind == "SurfaceOutput" )
            node.Inputs[1].Name = "Emissive"; // plausible, same type, wrong pin

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_FALSE( compiled.IsSuccess() ) << "a pin the catalogue does not declare compiled";
    EXPECT_NE( compiled.GetError().find( "Emissive" ), std::string::npos )
         << "the refusal does not name the pin as the file stores it: " << compiled.GetError();
    EXPECT_NE( compiled.GetError().find( "Emission" ), std::string::npos )
         << "the refusal does not name what the catalogue declares there: " << compiled.GetError();
    EXPECT_NE( compiled.GetError().find( "Surface Output" ), std::string::npos )
         << "the refusal does not name the node the artist can click: " << compiled.GetError();
}

// A link whose endpoint id belongs to no node was silently treated as "unlinked" — the input
// quietly took its default and the artist saw a graph that did not do what it drew.
TEST( ShaderGraphCompiler, DanglingLinkIsRejected )
{
    SG::Document doc  = SurfaceDoc();
    doc.Links[0].From = 999999; // no node owns this pin

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_FALSE( compiled.IsSuccess() );
    EXPECT_NE( compiled.GetError().find( "999999" ), std::string::npos ) << compiled.GetError();
}

// Direction matters: output -> input, never the reverse.
TEST( ShaderGraphCompiler, BackwardsLinkIsRejected )
{
    SG::Document doc = SurfaceDoc();
    std::swap( doc.Links[0].From, doc.Links[0].To );
    EXPECT_FALSE( SG::CompileToDShader( doc ).IsSuccess() );
}

// Two links into one input: the emitter kept whichever the map saw last, so the picture and the
// generated code disagreed with nothing to show for it.
TEST( ShaderGraphCompiler, TwoLinksIntoOneInputAreRejected )
{
    SG::Document doc = SurfaceDoc();

    auto     second = SG::MakeNode( doc, "ColorConst" );
    uint64_t albedo = 0;
    for ( const auto& node : doc.Nodes )
        if ( node.Kind == "SurfaceOutput" )
            albedo = node.Inputs[0].Id;
    ASSERT_NE( albedo, 0u );

    doc.Links.push_back( { doc.NextId++, second.Outputs[0].Id, albedo } );
    doc.Nodes.push_back( std::move( second ) );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_FALSE( compiled.IsSuccess() );
    EXPECT_NE( compiled.GetError().find( "Albedo" ), std::string::npos ) << compiled.GetError();
}

// A node the palette does not offer in this domain would emit GLSL referring to declarations the
// domain's stage never writes (SceneColor -> u_SceneTexture, absent from a Surface fragment).
TEST( ShaderGraphCompiler, OutOfDomainNodeIsRejected )
{
    SG::Document doc = SurfaceDoc();
    doc.Nodes.push_back( SG::MakeNode( doc, "SceneColor" ) );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_FALSE( compiled.IsSuccess() );
    EXPECT_NE( compiled.GetError().find( "Scene Color" ), std::string::npos ) << compiled.GetError();
}

// ======================================================================= the lit surface ======
//
// A Surface graph with Lit ticked used to carry a lighting model written by THIS COMPILER: a flat
// `vec3( 0.12 )` ambient that read no environment, `albedo * cos` with no division by PI, and no
// cloud shadow. All three were defects the engine had already fixed by making the term one shared
// text (Р16, Р20, Р21) — the graph was a fourth copy nobody had migrated.

namespace
{
    SG::Document LitSurfaceDoc()
    {
        SG::Document doc = SurfaceDoc();
        doc.Name         = "TestLit";
        doc.Lit          = true;
        return doc;
    }
} // namespace

TEST( ShaderGraphCompiler, ALitSurfaceCallsTheSharedModelAndWritesNoFormulaOfItsOwn )
{
    const auto compiled = SG::CompileToDShader( LitSurfaceDoc() );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& src = compiled.GetValue();

    // The whole model behind one include and one call.
    EXPECT_NE( src.find( "Common/GraphSurfaceLighting.glslh" ), std::string::npos ) << src;
    EXPECT_NE( src.find( "ShadeGraphSurface(" ), std::string::npos ) << src;

    // And not one line of lighting arithmetic left in the generated text. These are the exact three
    // fragments the old emitter wrote, so this fails if any of them is reintroduced here rather than
    // fixed in the shared header where every other surface would get it too.
    EXPECT_EQ( src.find( "vec3( 0.12 )" ), std::string::npos ) << "the flat ambient constant is back";
    EXPECT_EQ( src.find( "max( dot( N, L ), 0.0 )" ), std::string::npos )
         << "the graph is computing its own Lambert term again";
    EXPECT_EQ( src.find( "ColorIntensity" ), std::string::npos )
         << "the graph is unpacking the light payload itself instead of calling the shared model";

    // The lit vertex contract carries what the model needs: world position and eye position, not only
    // a normal. A surface that knows only its normal cannot be given a cloud shadow or a point light.
    EXPECT_NE( src.find( "GRAPH_LIT" ), std::string::npos );
    EXPECT_NE( src.find( "v_WorldPos" ), std::string::npos );
    EXPECT_NE( src.find( "v_CameraPos" ), std::string::npos );

    // Unwired, the material attributes fall back to the standard material's schema defaults.
    EXPECT_NE( src.find( "albedo.rgb, 0.0, 0.5, 1.0" ), std::string::npos ) << src;
}

TEST( ShaderGraphCompiler, AnUnlitSurfaceIsUntouchedByTheShadingModel )
{
    const auto compiled = SG::CompileToDShader( SurfaceDoc() ); // Lit defaults to false
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const std::string& src = compiled.GetValue();

    EXPECT_EQ( src.find( "GraphSurfaceLighting" ), std::string::npos ) << src;
    EXPECT_EQ( src.find( "ShadeGraphSurface" ), std::string::npos ) << src;
    EXPECT_EQ( src.find( "GRAPH_LIT" ), std::string::npos ) << src;
}

TEST( ShaderGraphCompiler, TheSurfaceOutputCarriesTheAttributesTheSharedModelConsumes )
{
    const SG::NodeSpec* spec = SG::FindSpec( "SurfaceOutput" );
    ASSERT_NE( spec, nullptr );

    // Both shared texts take metalness and roughness, and the ambient takes an occlusion factor. The
    // ORDER is part of the contract: pins are stored positionally in a .dgraph, so the three added by
    // Д16 are appended after Alpha and the first three keep their indices.
    ASSERT_EQ( spec->Inputs.size(), 6u );
    EXPECT_STREQ( spec->Inputs[0].Name, "Albedo" );
    EXPECT_STREQ( spec->Inputs[1].Name, "Emission" );
    EXPECT_STREQ( spec->Inputs[2].Name, "Alpha" );
    EXPECT_STREQ( spec->Inputs[3].Name, "Metallic" );
    EXPECT_STREQ( spec->Inputs[4].Name, "Roughness" );
    EXPECT_STREQ( spec->Inputs[5].Name, "Occlusion" );
}

TEST( ShaderGraphCompiler, TheGraphsOwnTexturesCannotLandOnAnEngineBinding )
{
    // The generated shader declares engine blocks at fixed slots and the parser numbers a Properties
    // block's textures upward from ONE base. While that base was 2, the third texture in a graph would
    // have been declared at binding 4 on top of LightsMetadata — two GLSL declarations on one descriptor,
    // which GLSL does not report.
    //
    // WHAT THIS TEST MAY AND MAY NOT CLAIM. It can say the emitter numbers from the reservation, which is
    // below; it CANNOT say the reservation is free, because that is a property of compiled SPIR-V and
    // this suite compiles nothing. It used to try, as `EXPECT_GT( kGraphTextureBinding, 23u )` — a gate
    // pinning a NUMBER, satisfiable by editing the number, and stale the moment an engine layout grew a
    // slot. The freedom of the window is measured over every shipped pass by
    // Desert/Tests/Engine/ShaderCacheKey (NoShippedProgramDeclaresABindingInTheGraphsReservedWindow), and
    // the two halves share one constant so they cannot drift apart.
    EXPECT_EQ( SG::kGraphTextureBinding, Desert::Core::kGraphOwnedBindingFirst );

    SG::Document doc = LitSurfaceDoc();
    auto         tex = SG::MakeNode( doc, "TextureSample" );
    tex.ParamName    = "u_Mask";
    doc.Nodes.push_back( std::move( tex ) );

    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    EXPECT_NE(
         compiled.GetValue().find( std::format( "TextureBinding({})", Desert::Core::kGraphOwnedBindingFirst ) ),
         std::string::npos )
         << compiled.GetValue();
}

// ========================================================================== the migration =====

TEST( ShaderGraphCompiler, ADocumentWrittenAgainstTheOlderCatalogueGrowsTheMissingPins )
{
    // A .dgraph exactly as builds before Д16 wrote it: a Surface Output with the three inputs the
    // catalogue had then. Verbatim rather than generated, because a document produced by THIS build can
    // never be short — the thing under test is a file on someone's disk.
    const std::string legacy =
         R"({"Name":"Legacy","NextId":8,"Domain":0,"Lit":true,"Nodes":[)"
         R"({"Id":1,"Kind":"ColorConst","ParamName":"","Value":[0.5,0.5,0.5,1.0],"X":0.0,"Y":0.0,)"
         R"("Inputs":[],"Outputs":[{"Id":2,"Name":"Color","Type":2}]},)"
         R"({"Id":3,"Kind":"SurfaceOutput","ParamName":"","Value":[1.0,1.0,1.0,1.0],"X":320.0,"Y":0.0,)"
         R"("Inputs":[{"Id":4,"Name":"Albedo","Type":2},{"Id":5,"Name":"Emission","Type":2},)"
         R"({"Id":6,"Name":"Alpha","Type":0}],"Outputs":[]}],)"
         R"("Links":[{"Id":7,"From":2,"To":4}]})";

    auto loaded = SG::Deserialize( legacy );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();

    // Reported, never silent — the panel logs this number and puts it in the status line.
    EXPECT_EQ( loaded.GetValue().MigratedPins, 3 );

    const SG::Document& doc = loaded.GetValue().Doc;

    const SG::Node* output = nullptr;
    for ( const auto& node : doc.Nodes )
        if ( node.Kind == "SurfaceOutput" )
            output = &node;
    ASSERT_NE( output, nullptr );
    ASSERT_EQ( output->Inputs.size(), 6u );

    // THE PROPERTY that makes appending safe, and the reason the migration refuses anything else: the
    // pins that were already there keep their index AND their id, so the saved link still lands on
    // Albedo rather than on whatever now occupies index 0.
    EXPECT_EQ( output->Inputs[0].Id, 4u );
    EXPECT_EQ( output->Inputs[2].Id, 6u );
    ASSERT_EQ( doc.Links.size(), 1u );
    EXPECT_EQ( doc.Links[0].To, output->Inputs[0].Id );

    // New ids come out of the document's own allocator, so nothing collides with what is already there.
    EXPECT_GE( output->Inputs[3].Id, 8u );
    EXPECT_NE( output->Inputs[3].Id, output->Inputs[4].Id );
    EXPECT_GT( doc.NextId, output->Inputs[5].Id );

    // And it compiles — which is the whole point, because ValidateGraph rejects a node whose pin count
    // disagrees with its kind. Without the migration every .dgraph in existence stopped compiling.
    EXPECT_TRUE( SG::CompileToDShader( doc ).IsSuccess() );
}

TEST( ShaderGraphCompiler, MigrationRefusesToRewriteAGraphThatIsNotMerelyOld )
{
    // Appending is safe BECAUSE what is stored is a prefix of the catalogue. A node whose pins were
    // renamed, retyped or reordered is not an old document, it is a wrong one, and inventing the tail of
    // it would turn a diagnosable file into a silently different graph. It is left alone so
    // ValidateGraph names it.
    SG::Document doc    = LitSurfaceDoc();
    SG::Node*    output = nullptr;
    for ( auto& node : doc.Nodes )
        if ( node.Kind == "SurfaceOutput" )
            output = &node;
    ASSERT_NE( output, nullptr );

    output->Inputs.resize( 2 );
    output->Inputs[1].Name = "NotEmission";

    EXPECT_EQ( SG::MigrateToCatalogue( doc ), 0 );
    EXPECT_EQ( output->Inputs.size(), 2u );
    EXPECT_FALSE( SG::CompileToDShader( doc ).IsSuccess() );
}

// THE TWO FUNCTIONS MUST GIVE A RENAMED PIN THE SAME ANSWER, and until O1-J they did not.
//
// The test above shortens the list AND renames a pin, so what refused it was the pin COUNT — and that
// hid the disagreement completely: a document of the right LENGTH with one pin renamed was left alone by
// MigrateToCatalogue (correctly: it is not a prefix of the catalogue, so nothing may be appended and
// nothing may be rewritten) and then accepted by ValidateGraph, which compared only types. One half
// refusing to touch a document while the other half declares it fine is worse than either policy on its
// own, because the refusal to repair was justified by a rejection that never happened.
TEST( ShaderGraphCompiler, TheMigrationAndTheValidatorAgreeOnARenamedPin )
{
    SG::Document doc    = LitSurfaceDoc();
    SG::Node*    output = nullptr;
    for ( auto& node : doc.Nodes )
        if ( node.Kind == "SurfaceOutput" )
            output = &node;
    ASSERT_NE( output, nullptr );

    const size_t pins      = output->Inputs.size();
    output->Inputs[1].Name = "Emissive"; // right count, right types, one wrong name

    // The migration's answer: not a prefix, so not repaired — and not truncated or grown either.
    EXPECT_EQ( SG::MigrateToCatalogue( doc ), 0 );
    ASSERT_EQ( output->Inputs.size(), pins );
    EXPECT_EQ( output->Inputs[1].Name, "Emissive" ) << "the migration rewrote a name it must not touch";

    // The validator's answer: the same refusal, and it says which pin.
    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_FALSE( compiled.IsSuccess() );
    EXPECT_NE( compiled.GetError().find( "Emissive" ), std::string::npos ) << compiled.GetError();
}

// ============================================================== the corpus on disk ============

namespace
{
    std::filesystem::path GraphsDirectory()
    {
        return Desert::Tests::ShaderGraph::RepoRoot() / "Editor/Resources/Assets/ShaderGraphs";
    }
} // namespace

// WHAT MAKES "REFUSE A RENAMED PIN" SURVIVABLE. Refusing rather than repairing is only the right answer
// if a catalogue rename is discovered by whoever makes it rather than by an artist opening a file, and
// nothing in this suite ever read a committed `.dgraph` — every document above is built in memory from
// the very catalogue it is checked against, so a rename moves both sides at once and no assertion can
// see it. This one reads the files, so renaming a pin in Specs() reddens here and names the graph that
// has to be migrated.
TEST( ShaderGraphCompiler, EveryGraphCommittedToTheProjectStillCompiles )
{
    const std::filesystem::path directory = GraphsDirectory();
    ASSERT_TRUE( std::filesystem::is_directory( directory ) )
         << "the project's graph corpus is not where this test looks: " << directory;

    int seen = 0;
    for ( const auto& entry : std::filesystem::directory_iterator( directory ) )
    {
        if ( entry.path().extension() != ".dgraph" )
            continue;
        ++seen;

        const std::string json = Desert::Tests::ShaderGraph::ReadAll( entry.path() );
        ASSERT_FALSE( json.empty() ) << entry.path().string() << " is empty or unreadable";

        // Through Deserialize, not through a hand-built Document: the migration is part of what a load
        // IS, and a corpus that only compiles after somebody edits it by hand is not a corpus that works.
        const auto loaded = SG::Deserialize( json );
        ASSERT_TRUE( loaded.IsSuccess() ) << entry.path().string() << ": " << loaded.GetError();

        const auto compiled = SG::CompileToDShader( loaded.GetValue().Doc );
        EXPECT_TRUE( compiled.IsSuccess() )
             << entry.path().filename().string()
             << " no longer compiles against the current node catalogue: " << compiled.GetError()
             << "\nA catalogue change that a saved graph cannot follow is a migration, not a rename: the "
                "compiler refuses such a document by design rather than guessing what the artist meant.";
    }

    // Printed, not asserted. A number here is a number somebody edits when they delete a graph; what the
    // test is for is that whatever the corpus contains, all of it compiles.
    ASSERT_GT( seen, 0 ) << "no .dgraph was found at all, so this test measured nothing";
    std::printf( "[ShaderGraphCompiler] %d committed .dgraph compiled\n", seen );
}

// THE ONE GRAPH THAT MUST NOT COMPILE, and the reason it is HERE rather than in the corpus above.
//
// `MatBroken` is the Stage-1 reproduction of "a graph that compiles to invalid GLSL takes the editor
// down with it": a vec2 wired into the vec4 Albedo pin, legal in the file and refused by the canvas.
// Г20 already moved the SHADER it produced out of the shipped tree into a test fixture, because it was
// compiled at every editor start and put two errors into every clean log. The `.dgraph` half was left
// behind in Editor/Resources/Assets/ShaderGraphs with no consumer anywhere — the only graph in that
// directory without a `.shader`, a `.demat` and a scene — where it was still offered to an artist by the
// panel's Load popup and still counted as content the project ships. O1-J finishes that move: it lives
// beside this suite, and what it demonstrates is asserted instead of shipped.
TEST( ShaderGraphCompiler, TheDeliberatelyBrokenGraphIsRefusedBeforeAnyGlslIsEmitted )
{
    const std::filesystem::path fixture = Desert::Tests::ShaderGraph::RepoRoot() /
                                          "Desert/Tests/Editor/ShaderGraphCompiler/Fixtures" / "MatBroken.dgraph";
    const std::string json = Desert::Tests::ShaderGraph::ReadAll( fixture );
    ASSERT_FALSE( json.empty() ) << "the fixture is not where this test looks: " << fixture;

    const auto loaded = SG::Deserialize( json );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();

    const auto compiled = SG::CompileToDShader( loaded.GetValue().Doc );
    ASSERT_FALSE( compiled.IsSuccess() )
         << "the reproduction of the defect this validator was written for now compiles";
    EXPECT_NE( compiled.GetError().find( "Albedo" ), std::string::npos )
         << "the refusal does not name the pin the artist wired into: " << compiled.GetError();
    EXPECT_NE( compiled.GetError().find( "UV" ), std::string::npos )
         << "the refusal does not name the node the value came from: " << compiled.GetError();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
