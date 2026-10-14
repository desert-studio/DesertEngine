// VFX-03g. The sprite renderer's bindings to an emitter's compiled layout (UE Niagara renderer bindings):
// Position / Color / SpriteSize resolve to the attribute's FloatStart in THAT layout, and an attribute the layout
// lacks, or holds with another component count, stays unbound (the renderer default) and is named once.
#include <gtest/gtest.h>

#include <Engine/Graphic/Systems/Scene/Particles/ParticleSpriteBindings.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    namespace Sys = Desert::Graphic::System;
    using Desert::VFX::VFXAttributeLayout;
    using Desert::VFX::VFXDataSetLayout;

    VFXAttributeLayout Floats( const std::string& name, const uint32_t start, const uint32_t count )
    {
        VFXAttributeLayout attribute;
        attribute.Name       = name;
        attribute.FloatStart = start;
        attribute.FloatCount = count;
        return attribute;
    }
} // namespace

// Red when a binding reads a fixed offset instead of the layout's (Position is not first here), when an attribute
// is matched by position rather than name, or when the starts of two layouts with the same attributes in another
// order come out equal.
TEST( VFXSpriteBindings, EachAttributeBindsToItsStartInTheEmittersLayout )
{
    VFXDataSetLayout layout;
    layout.Attributes           = { Floats( "Age", 0, 1 ), Floats( "Color", 1, 4 ), Floats( "Velocity", 5, 3 ),
                                    Floats( "Position", 8, 3 ), Floats( "SpriteSize", 11, 2 ) };
    layout.TotalFloatComponents = 13;

    const Sys::ParticleSpriteBindings bindings = Sys::ResolveParticleSpriteBindings( layout );
    EXPECT_TRUE( bindings.Unbound.empty() ) << bindings.Unbound.front();
    EXPECT_EQ( bindings.StartOf( Sys::ParticleSpriteAttribute::Position ), 8u );
    EXPECT_EQ( bindings.StartOf( Sys::ParticleSpriteAttribute::Color ), 1u );
    EXPECT_EQ( bindings.StartOf( Sys::ParticleSpriteAttribute::SpriteSize ), 11u );

    VFXDataSetLayout other;
    other.Attributes = { Floats( "Position", 0, 3 ), Floats( "SpriteSize", 3, 2 ), Floats( "Color", 5, 4 ) };
    other.TotalFloatComponents               = 9;
    const Sys::ParticleSpriteBindings second = Sys::ResolveParticleSpriteBindings( other );
    EXPECT_EQ( second.StartOf( Sys::ParticleSpriteAttribute::Position ), 0u );
    EXPECT_EQ( second.StartOf( Sys::ParticleSpriteAttribute::SpriteSize ), 3u );
    EXPECT_EQ( second.StartOf( Sys::ParticleSpriteAttribute::Color ), 5u );
}

// Red when a missing attribute binds to component 0 (it would read another attribute's data), when a wrong-sized
// one binds, or when either goes unreported.
TEST( VFXSpriteBindings, AMissingOrMisSizedAttributeUsesTheRendererDefaultAndIsNamed )
{
    VFXDataSetLayout layout;
    layout.Attributes           = { Floats( "Position", 0, 3 ), Floats( "SpriteSize", 3, 1 ) };
    layout.TotalFloatComponents = 4;

    const Sys::ParticleSpriteBindings bindings = Sys::ResolveParticleSpriteBindings( layout );
    EXPECT_EQ( bindings.StartOf( Sys::ParticleSpriteAttribute::Position ), 0u );
    EXPECT_EQ( bindings.StartOf( Sys::ParticleSpriteAttribute::Color ), Sys::kParticleSpriteUnbound );
    EXPECT_EQ( bindings.StartOf( Sys::ParticleSpriteAttribute::SpriteSize ), Sys::kParticleSpriteUnbound );
    ASSERT_EQ( bindings.Unbound.size(), 2u );
    EXPECT_EQ( bindings.Unbound[0].rfind( "Color:", 0 ), 0u ) << bindings.Unbound[0];
    EXPECT_EQ( bindings.Unbound[1].rfind( "SpriteSize:", 0 ), 0u ) << bindings.Unbound[1];
}

// VFX-03h. The sprite vertex path and the view sort read a stack emitter THROUGH these bindings: the stack branch
// of each reads Position / Color / SpriteSize from the pool's float components at the emitter's source row starts
// (Common/ParticleSpriteSource.glslh) and touches no field of the fixed Particle record, and the shader's renderer
// defaults are the C++ ones. Red when a stack branch reads u_Particles / PosSize / a Particle field, when a branch
// stops reading an attribute through its start, when the stack branch is gone, or when the defaults drift apart.
namespace
{
    std::string ReadShaderText( const char* relative )
    {
        const std::filesystem::path root = std::filesystem::path( __FILE__ )
                                                .parent_path()
                                                .parent_path()
                                                .parent_path()
                                                .parent_path()
                                                .parent_path();
        std::ifstream file( ( root / "Editor/Resources/Shaders" / relative ).string() );
        EXPECT_TRUE( file.is_open() ) << relative;
        return std::string( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    }

    // The text of the branch taken for a stack source: from the STACK test to the `else` (or `:`) after it.
    std::string StackBranch( const std::string& text, const std::string& end )
    {
        const size_t begin = text.find( "== PARTICLE_SPRITE_SOURCE_STACK" );
        if ( begin == std::string::npos )
            return {};
        const size_t stop = text.find( end, begin );
        return text.substr( begin, stop == std::string::npos ? std::string::npos : stop - begin );
    }
} // namespace

TEST( VFXSpriteBindings, TheSpriteVertexPathAndTheSortReadAStackThroughItsBindingsNotTheParticleRecord )
{
    const std::string vertex = ReadShaderText( "Mesh/Surface/Vertex_ParticleSprite.glslh" );
    const std::string branch = StackBranch( vertex, "    else" );
    ASSERT_FALSE( branch.empty() ) << "the sprite vertex path has no stack branch";
    for ( const char* record : { "u_Particles", "PosSize", "VelLife", "p.", "Particle " } )
        EXPECT_EQ( branch.find( record ), std::string::npos ) << "the stack branch reads the record: " << record;
    for ( const char* read :
          { "PARTICLE_SPRITE_VEC3( u_Floats, source, source.Starts.x",
            "PARTICLE_SPRITE_VEC4( u_Floats, source, source.Starts.y",
            "PARTICLE_SPRITE_VEC2( u_Floats, source, source.Starts.z", "u_SlotState[index].Alive" } )
        EXPECT_NE( branch.find( read ), std::string::npos ) << "the stack branch does not read " << read;

    const std::string sort       = ReadShaderText( "Programs/Particles/ParticleSort.shader" );
    const std::string sortBranch = StackBranch( sort, ":" );
    ASSERT_FALSE( sortBranch.empty() ) << "the sort has no stack branch";
    EXPECT_NE( sortBranch.find( "PARTICLE_SPRITE_VEC3( u_Floats, source, source.Starts.x" ), std::string::npos );
    EXPECT_EQ( sortBranch.find( "u_Particles" ), std::string::npos );

    const std::string source = ReadShaderText( "Common/ParticleSpriteSource.glslh" );
    EXPECT_NE( source.find( "#define PARTICLE_SPRITE_UNBOUND 0xFFFFFFFFu" ), std::string::npos );
    EXPECT_EQ( Sys::kParticleSpriteUnbound, 0xFFFFFFFFu );
    EXPECT_NE( source.find( "#define PARTICLE_SPRITE_DEFAULT_POSITION vec3( 0.0 )" ), std::string::npos );
    EXPECT_EQ( Sys::kParticleSpriteDefaultPosition, glm::vec3( 0.0f ) );
    EXPECT_NE( source.find( "#define PARTICLE_SPRITE_DEFAULT_COLOR vec4( 1.0 )" ), std::string::npos );
    EXPECT_EQ( Sys::kParticleSpriteDefaultColor, glm::vec4( 1.0f ) );
    EXPECT_NE( source.find( "#define PARTICLE_SPRITE_DEFAULT_SIZE vec2( 10.0 )" ), std::string::npos );
    EXPECT_EQ( Sys::kParticleSpriteDefaultSize, glm::vec2( 10.0f ) );
    EXPECT_NE( source.find( "#define PARTICLE_SPRITE_SOURCE_STACK 1u" ), std::string::npos );
    EXPECT_EQ( Sys::kParticleSpriteSourceStack, 1u );

    // The row a stack draws with: its range and its starts, in the glslh's field order.
    Sys::ParticleSpriteBindings bindings;
    bindings.FloatStart                    = { 8u, Sys::kParticleSpriteUnbound, 3u, 5u };
    const Sys::ParticleSpriteSourceGpu row = Sys::ParticleSpriteSourceOf( bindings, 640u, 256u, 1024u );
    EXPECT_EQ( row.Layout, glm::uvec4( 640u, 256u, 1024u, Sys::kParticleSpriteSourceStack ) );
    EXPECT_EQ( row.Starts, glm::uvec4( 8u, Sys::kParticleSpriteUnbound, 3u, 5u ) );
    // VFX-03i: the relative time is the NormalizedAge binding (Starts.w), the renderer default when unbound.
    EXPECT_NE( source.find( "#define PARTICLE_SPRITE_DEFAULT_NORMALIZED_AGE 0.0" ), std::string::npos );
    EXPECT_EQ( Sys::kParticleSpriteDefaultNormalizedAge, 0.0f );
}
