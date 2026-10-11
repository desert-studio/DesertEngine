// VFX-08: THE PARTICLE SPRITE USAGE of a surface template (UE: a material's bUsedWithParticleSprites).
//
// A template that says `Usage ParticleSprites` is compiled for ONE more vertex factory: the cell
// `ParticleSprite.Forward`, built from the sprite vertex path (Mesh/Surface/Vertex_ParticleSprite.glslh: the
// particle's colour, relative time and random into SurfaceInput.Particle) and the sprite pass header
// (Pass_ParticleSprite.glslh: unlit, Depth Fade over the scene depth). These tests hold the parser to that
// contract: the cell exists exactly when the usage is declared, it comes after every mesh cell (so no mesh cell
// index moves), its stages are told they are a sprite (and translucent when the template is), and the shader-map
// key hashes the two new headers.

#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace P = Desert::Core::Preprocess;
using Desert::Core::Formats::ShaderStage;

namespace
{
    std::string Template( const std::string& settings )
    {
        return "Shader \"SpriteTemplate\"\n{\n    Domain Surface\n\n" + settings +
               "    ShadingModel Unlit\n\n"
               "    Surface\n    {\n"
               "        SurfaceOutput EvaluateSurface( SurfaceInput i )\n        {\n"
               "            SurfaceOutput s = DefaultSurfaceOutput();\n"
               "            s.BaseColor = vec3( 0.0 );\n"
               "            s.Emissive = i.Particle.Color.rgb;\n"
               "            s.Opacity = DesertDepthFade( i.WorldPosition, i.Particle.Color.a, 50.0 );\n"
               "            return s;\n        }\n    }\n}\n";
    }

    std::string SpriteCell()
    {
        return P::SurfaceCellName( P::kSurfaceParticleSpritePath, P::kSurfaceParticleSpritePass );
    }

    std::vector<std::string> MeshCells()
    {
        std::vector<std::string> cells;
        for ( const std::string_view path : P::kSurfaceVertexPaths )
            for ( const std::string_view pass : P::kSurfaceCellPasses )
                cells.push_back( P::SurfaceCellName( path, pass ) );
        return cells;
    }
} // namespace

TEST( SurfaceTemplate, ParticleSpriteUsageAddsOneSpriteCellAfterEveryMeshCell )
{
    const auto without = P::DShaderParser::Parse( Template( "" ) );
    ASSERT_TRUE( without.IsSuccess() ) << without.GetError();
    EXPECT_FALSE( without.GetValue().Surface.UsedWithParticleSprites );
    EXPECT_EQ( without.GetValue().Surface.Cells, MeshCells() ) << "no usage, no sprite cell";

    const auto with = P::DShaderParser::Parse( Template( "    Usage ParticleSprites\n" ) );
    ASSERT_TRUE( with.IsSuccess() ) << with.GetError();
    const auto& parsed = with.GetValue();
    EXPECT_TRUE( parsed.Surface.UsedWithParticleSprites );
    std::vector<std::string> expected = MeshCells();
    expected.push_back( SpriteCell() );
    EXPECT_EQ( parsed.Surface.Cells, expected ) << "the sprite cell is appended: no mesh cell index moves";

    const P::DShaderPass* cell = parsed.FindPass( SpriteCell() );
    ASSERT_NE( cell, nullptr );
    const std::string define = "#define " + std::string( P::kSurfaceParticleSpriteDefine ) + " 1";
    for ( const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment } )
    {
        ASSERT_TRUE( cell->Stages.contains( stage ) );
        EXPECT_NE( cell->Stages.at( stage ).find( define ), std::string::npos )
             << "both stages of the sprite cell must know they are a sprite (Depth Fade reads the scene depth "
                "there)";
    }
    EXPECT_NE( cell->Stages.at( ShaderStage::Vertex ).find( "Vertex_ParticleSprite.glslh" ), std::string::npos );
    EXPECT_NE( cell->Stages.at( ShaderStage::Fragment ).find( P::kSurfaceParticleSpritePassInclude ),
               std::string::npos )
         << "the sprite pass header composites the particle, not the mesh Forward header";

    // No mesh cell is told it is a sprite.
    const P::DShaderPass* mesh = parsed.FindPass( std::string( P::kSurfaceDefaultCell ) );
    ASSERT_NE( mesh, nullptr );
    EXPECT_EQ( mesh->Stages.at( ShaderStage::Fragment ).find( define ), std::string::npos );
}

TEST( SurfaceTemplate, TranslucentSpriteCellIsToldItBlends )
{
    const auto parsed =
         P::DShaderParser::Parse( Template( "    BlendMode Translucent\n    Usage ParticleSprites\n" ) );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const P::DShaderPass* cell = parsed.GetValue().FindPass( SpriteCell() );
    ASSERT_NE( cell, nullptr ) << "a translucent template keeps its sprite cell (Forward is its one pass)";
    const std::string define = "#define " + std::string( P::kSurfaceTranslucentDefine ) + " 1";
    EXPECT_NE( cell->Stages.at( ShaderStage::Fragment ).find( define ), std::string::npos );

    const auto opaque = P::DShaderParser::Parse( Template( "    Usage ParticleSprites\n" ) );
    ASSERT_TRUE( opaque.IsSuccess() );
    EXPECT_EQ( opaque.GetValue().FindPass( SpriteCell() )->Stages.at( ShaderStage::Fragment ).find( define ),
               std::string::npos );
}

TEST( SurfaceTemplate, UnknownUsageAndUsageWithoutASurfaceAreRefused )
{
    const auto unknown = P::DShaderParser::Parse( Template( "    Usage Ribbons\n" ) );
    ASSERT_FALSE( unknown.IsSuccess() );
    EXPECT_NE( unknown.GetError().find( "Ribbons" ), std::string::npos ) << unknown.GetError();

    const auto bare = P::DShaderParser::Parse( "Shader \"NoSurface\"\n{\n    Usage ParticleSprites\n"
                                               "    Vertex\n    {\n        void main() {}\n    }\n}\n" );
    EXPECT_FALSE( bare.IsSuccess() );
}

TEST( SurfaceTemplate, ShaderMapKeyHashesTheSpriteHeaders )
{
    const std::vector<std::string> includes = P::SurfaceTemplateIncludes();
    const auto                     has      = [&]( const std::string& header )
    { return std::find( includes.begin(), includes.end(), header ) != includes.end(); };
    EXPECT_TRUE( has( P::SurfaceVertexInclude( P::kSurfaceParticleSpritePath ) ) );
    EXPECT_TRUE( has( std::string( P::kSurfaceParticleSpritePassInclude ) ) );
}
