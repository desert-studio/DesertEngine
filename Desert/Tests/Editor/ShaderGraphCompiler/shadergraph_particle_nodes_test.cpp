// The Particles nodes (VFX-08g; UE's Particle Color / Particle SubUV / Particle Random / Particle Relative Time).
// Each reads one field of SurfaceInput.Particle, which only the ParticleSprite cell of a `Usage ParticleSprites`
// template fills. Held here: each compiles in a sprite material and its generated code reads its field; each is
// refused by name (node and material) in a material without the usage; the sprite cell's stage inputs carry every
// field the nodes read, from the vertex stage that writes them; the palette lists them under the Particles
// category.

#include <gtest/gtest.h>

#include "graph_test_tree.hpp"

#include <ShaderGraph.hpp> // editor: the graph document + compiler under test

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp> // engine: the real parser

#include <regex>
#include <set>
#include <string>
#include <vector>

namespace SG = Desert::Editor::ShaderGraph;
namespace P  = Desert::Core::Preprocess;
using Desert::Core::Formats::ShaderStage;

namespace
{
    // One Particles node wired into the Surface Output pin its output type fits.
    struct ParticleProbe
    {
        const char* Kind;
        const char* OutputPin;  // the node's output that is wired
        size_t      SurfacePin; // SurfaceOutput input index: 0 Albedo (Color), 2 Alpha (Float)
        const char* Reads;      // the text the generated code must contain
        const char* Field;      // the SurfaceInput.Particle field the node reads
    };

    const std::vector<ParticleProbe>& Probes()
    {
        static const std::vector<ParticleProbe> s_Probes = {
             { "ParticleColor", "RGBA", 0, "= i.Particle.Color;", "Color" },
             { "ParticleRandom", "Random", 2, "= i.Particle.Random;", "Random" },
             { "ParticleRelativeTime", "RelativeTime", 2, "= i.Particle.RelativeTime;", "RelativeTime" },
             // SubImage hands DesertParticleSubUV the whole particle input; it steps the flipbook by RelativeTime.
             { "ParticleSubImage", "Blend", 2, "= DesertParticleSubUV( i.Particle, v_UV, 1.0, 1.0 );",
               "RelativeTime" },
        };
        return s_Probes;
    }

    SG::Document ProbeDoc( const ParticleProbe& probe, const bool usedWithSprites )
    {
        SG::Document doc;
        doc.Name                    = "ParticleProbe";
        doc.UsedWithParticleSprites = usedWithSprites;

        SG::Node node   = SG::MakeNode( doc, probe.Kind );
        SG::Node output = SG::MakeNode( doc, "SurfaceOutput" );
        uint64_t from   = 0;
        for ( const auto& pin : node.Outputs )
            if ( pin.Name == probe.OutputPin )
                from = pin.Id;
        EXPECT_NE( from, 0u ) << probe.Kind << " has no output '" << probe.OutputPin << "'";
        doc.Links.push_back( { doc.NextId++, from, output.Inputs[probe.SurfacePin].Id } );
        doc.Nodes.push_back( std::move( node ) );
        doc.Nodes.push_back( std::move( output ) );
        return doc;
    }

    std::string SpriteCell()
    {
        return P::SurfaceCellName( P::kSurfaceParticleSpritePath, P::kSurfaceParticleSpritePass );
    }

    std::string ShaderText( const char* relative )
    {
        return Desert::Tests::ShaderGraph::ReadAll( Desert::Tests::ShaderGraph::RepoRoot() /
                                                    "Editor/Resources/Shaders" / relative );
    }
} // namespace

TEST( ShaderGraphCompiler, EachParticleNodeCompilesInASpriteMaterialAndReadsItsInput )
{
    for ( const ParticleProbe& probe : Probes() )
    {
        const auto compiled = SG::CompileToDShader( ProbeDoc( probe, true ) );
        ASSERT_TRUE( compiled.IsSuccess() ) << probe.Kind << ": " << compiled.GetError();
        const std::string& src = compiled.GetValue();
        EXPECT_NE( src.find( "    Usage ParticleSprites\n" ), std::string::npos ) << probe.Kind << "\n" << src;
        EXPECT_NE( src.find( probe.Reads ), std::string::npos )
             << probe.Kind << " must read " << probe.Reads << "\n"
             << src;

        const auto parsed = P::DShaderParser::Parse( src );
        ASSERT_TRUE( parsed.IsSuccess() ) << probe.Kind << ": " << parsed.GetError() << "\n" << src;
        EXPECT_TRUE( parsed.GetValue().Surface.UsedWithParticleSprites ) << probe.Kind;
        const P::DShaderPass* cell = parsed.GetValue().FindPass( SpriteCell() );
        ASSERT_NE( cell, nullptr ) << probe.Kind << ": no " << SpriteCell() << " cell";
        ASSERT_TRUE( cell->Stages.contains( ShaderStage::Fragment ) );
        EXPECT_NE( cell->Stages.at( ShaderStage::Fragment ).find( probe.Reads ), std::string::npos )
             << probe.Kind << ": the sprite cell's fragment stage must evaluate the node";
    }

    // SubImage's three outputs are the SurfaceSubUV members of the one variable, picked by pin name.
    SG::Document doc    = ProbeDoc( Probes()[3], true );
    SG::Node     tex    = SG::MakeNode( doc, "TextureSample" );
    tex.ParamName       = "Flipbook";
    const SG::Node* sub = &doc.Nodes[0];
    doc.Links.push_back( { doc.NextId++, sub->Outputs[1].Id, tex.Inputs[0].Id } ); // UV1 -> UV
    doc.Links.push_back( { doc.NextId++, tex.Outputs[0].Id, doc.Nodes[1].Inputs[0].Id } );
    doc.Nodes.push_back( std::move( tex ) );
    const auto compiled = SG::CompileToDShader( doc );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    EXPECT_NE( compiled.GetValue().find( ".UV1" ), std::string::npos ) << compiled.GetValue();
    EXPECT_NE( compiled.GetValue().find( ".Blend" ), std::string::npos ) << compiled.GetValue();
}

TEST( ShaderGraphCompiler, ParticleNodesAreRefusedByNameInAMaterialWithoutTheSpriteUsage )
{
    for ( const ParticleProbe& probe : Probes() )
    {
        const auto compiled = SG::CompileToDShader( ProbeDoc( probe, false ) );
        ASSERT_FALSE( compiled.IsSuccess() ) << probe.Kind << " compiled in a mesh-only material";
        const std::string& error = compiled.GetError();
        EXPECT_NE( error.find( SG::FindSpec( probe.Kind )->Title ), std::string::npos ) << error;
        EXPECT_NE( error.find( "'ParticleProbe'" ), std::string::npos ) << "the material is named: " << error;
        EXPECT_NE( error.find( "particle sprites" ), std::string::npos ) << error;
    }

    // The same graph without a Particles node is a mesh-only material that compiles with no sprite cell.
    SG::Document plain;
    plain.Name = "MeshOnly";
    plain.Nodes.push_back( SG::MakeNode( plain, "SurfaceOutput" ) );
    const auto compiled = SG::CompileToDShader( plain );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    EXPECT_EQ( compiled.GetValue().find( "Usage ParticleSprites" ), std::string::npos );
}

// The sprite cell's inputs, as its stages declare them: every field a node reads is assigned in
// Pass_ParticleSprite.glslh from a flat `in` at some location, and Vertex_ParticleSprite.glslh declares the same
// name as a flat `out` at the SAME location and writes it. A field the nodes read that the sprite pass never fills
// would be the mesh cells' neutral constant on a sprite too.
TEST( ShaderGraphCompiler, TheSpriteCellsInputsCarryEveryParticleFieldTheNodesRead )
{
    const std::string types  = ShaderText( "Mesh/Surface/SurfaceTypes.glslh" );
    const std::string pass   = ShaderText( "Mesh/Surface/Pass_ParticleSprite.glslh" );
    const std::string vertex = ShaderText( "Mesh/Surface/Vertex_ParticleSprite.glslh" );
    ASSERT_FALSE( types.empty() );
    ASSERT_FALSE( pass.empty() );
    ASSERT_FALSE( vertex.empty() );

    // SubImage reads RelativeTime through DesertParticleSubUV.
    EXPECT_NE( types.find( "clamp( p.RelativeTime" ), std::string::npos );

    std::set<std::string> fields;
    for ( const ParticleProbe& probe : Probes() )
        fields.insert( probe.Field );
    ASSERT_EQ( fields.size(), 3u );

    for ( const std::string& field : fields )
    {
        const std::regex member( "struct SurfaceParticleInput\\s*\\{[^}]*\\s" + field + ";" );
        EXPECT_TRUE( std::regex_search( types, member ) ) << "SurfaceParticleInput has no " << field;

        std::smatch      assigned;
        const std::regex fill( "i\\.Particle\\." + field + "\\s*=\\s*(v_\\w+)" );
        ASSERT_TRUE( std::regex_search( pass, assigned, fill ) )
             << "Pass_ParticleSprite.glslh never fills i.Particle." << field;
        const std::string varying = assigned[1].str();

        std::smatch      in;
        const std::regex inDecl( "layout\\(\\s*location\\s*=\\s*(\\d+)\\s*\\)\\s*in\\s+flat\\s+\\w+\\s+" +
                                 varying + "\\s*;" );
        ASSERT_TRUE( std::regex_search( pass, in, inDecl ) )
             << varying << " is not a stage input of the sprite pass";

        std::smatch      out;
        const std::regex outDecl( "layout\\(\\s*location\\s*=\\s*(\\d+)\\s*\\)\\s*out\\s+flat\\s+\\w+\\s+" +
                                  varying + "\\s*;" );
        ASSERT_TRUE( std::regex_search( vertex, out, outDecl ) )
             << varying << " is not a stage output of the sprite vertex path";
        EXPECT_EQ( in[1].str(), out[1].str() )
             << varying << ": the vertex writes one location, the pass reads another";
        EXPECT_TRUE( std::regex_search( vertex, std::regex( "\\n\\s*" + varying + "\\s*=" ) ) )
             << "the sprite vertex path never writes " << varying;
    }
}

TEST( ShaderGraphCompiler, ThePaletteListsTheParticleNodesUnderParticlesInTheSurfaceDomainOnly )
{
    for ( const ParticleProbe& probe : Probes() )
    {
        const SG::NodeSpec* spec = SG::FindSpec( probe.Kind );
        ASSERT_NE( spec, nullptr ) << probe.Kind;
        ASSERT_NE( spec->Category, nullptr ) << probe.Kind;
        EXPECT_EQ( std::string( spec->Category ), "Particles" ) << probe.Kind;
        EXPECT_TRUE( SG::SpecInDomain( *spec, SG::Domain::Surface ) ) << probe.Kind;
        EXPECT_FALSE( SG::SpecInDomain( *spec, SG::Domain::PostProcess ) ) << probe.Kind;
        EXPECT_FALSE( SG::SpecInDomain( *spec, SG::Domain::Volume ) ) << probe.Kind;
    }
    // The category and the usage rule are one set: every Particles node needs the sprite usage, and no other does.
    for ( const auto& spec : SG::Specs() )
        EXPECT_EQ( spec.Category && std::string( spec.Category ) == SG::kParticlesCategory,
                   SG::ReadsParticleInputs( spec.Kind ) )
             << spec.Kind;
}
