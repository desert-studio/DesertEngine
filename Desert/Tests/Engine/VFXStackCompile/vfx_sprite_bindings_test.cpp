// VFX-03g. The sprite renderer's bindings to an emitter's compiled layout (UE Niagara renderer bindings):
// Position / Color / SpriteSize resolve to the attribute's FloatStart in THAT layout, and an attribute the layout
// lacks, or holds with another component count, stays unbound (the renderer default) and is named once.
#include <gtest/gtest.h>

#include <Engine/Graphic/Systems/Scene/Particles/ParticleSpriteBindings.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

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
