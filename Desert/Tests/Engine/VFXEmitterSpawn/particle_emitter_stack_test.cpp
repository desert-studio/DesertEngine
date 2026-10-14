// VFX-03i: a ParticleEmitterComponent runs as its own compiled layout - the built-in stack
// VFX::ParticleEmitterSystem builds from its fields - on the stack path, so the sprite renderer reads it through
// its bindings like any `.dfx` emitter. Red when the built-in stack stops compiling (a module file or input
// renamed), when its layout loses an attribute the sprite renderer binds (Position / Color / SpriteSize), or when
// a field stops reaching its row.

#include <gtest/gtest.h>

#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/Systems/Scene/Particles/ParticleSpriteBindings.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <algorithm>

namespace
{
    using namespace Desert;

    TEST( ParticleEmitterStack, ALegacyEmitterCompilesToALayoutHoldingEverySpriteBinding )
    {
        ECS::ParticleEmitterData data;
        const auto               system   = VFX::ParticleEmitterSystem( data, glm::vec3( 100.0f, 0.0f, -50.0f ) );
        const auto               compiled = VFX::CompileEmitterStack( system, 0, VFX::EngineModuleDir() );
        ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();

        const auto bindings = Graphic::System::ResolveParticleSpriteBindings( compiled.GetValue().Layout );
        for ( const std::string& unbound : bindings.Unbound )
            ADD_FAILURE() << "the legacy emitter's layout leaves a sprite binding to the default: " << unbound;
        for ( const char* name :
              { "Position", "Velocity", "Age", "Lifetime", "NormalizedAge", "Color", "SpriteSize" } )
            EXPECT_NE( compiled.GetValue().Layout.Find( name ), nullptr ) << name;
        EXPECT_EQ( system.Emitters.at( 0 ).Capacity, static_cast<uint32_t>( data.MaxParticles ) );

        // The sprite's relative time comes through the NormalizedAge binding (row Starts.w), not a fixed record.
        const auto* age = compiled.GetValue().Layout.Find( "NormalizedAge" );
        ASSERT_NE( age, nullptr );
        EXPECT_EQ( bindings.StartOf( Graphic::System::ParticleSpriteAttribute::NormalizedAge ), age->FloatStart );
        EXPECT_EQ( Graphic::System::ParticleSpriteSourceOf( bindings, 0u, 16u, 0u ).Starts.w, age->FloatStart );
    }

    TEST( ParticleEmitterStack, AStackWithoutNormalizedAgeDrawsAtTheDefaultTimeAndIsNotReported )
    {
        VFX::VFXDataSetLayout layout; // no attribute at all
        const auto            bindings = Graphic::System::ResolveParticleSpriteBindings( layout );
        EXPECT_EQ( bindings.StartOf( Graphic::System::ParticleSpriteAttribute::NormalizedAge ),
                   Graphic::System::kParticleSpriteUnbound );
        EXPECT_EQ( bindings.Unbound.size(), 3u ) << "only Position / Color / SpriteSize are required bindings";
    }

    TEST( ParticleEmitterStack, EveryComponentSharesOneProgramAndItsFieldsReachTheRows )
    {
        ECS::ParticleEmitterData a;
        ECS::ParticleEmitterData b;
        b.StartSpeed  = 777.0f;
        b.WorldSpace  = false;
        const auto sa = VFX::ParticleEmitterSystem( a, glm::vec3( 0.0f ) );
        const auto sb = VFX::ParticleEmitterSystem( b, glm::vec3( 10.0f, 20.0f, 30.0f ) );
        const auto ca = VFX::CompileEmitterStack( sa, 0, VFX::EngineModuleDir() );
        const auto cb = VFX::CompileEmitterStack( sb, 0, VFX::EngineModuleDir() );
        ASSERT_TRUE( ca.IsSuccess() ) << ca.GetError();
        ASSERT_TRUE( cb.IsSuccess() ) << cb.GetError();
        EXPECT_EQ( ca.GetValue().Key, cb.GetValue().Key ) << "a field value moved the structure";

        const auto atlas = VFX::BuildCurveAtlas( sb );
        ASSERT_TRUE( atlas.IsSuccess() ) << atlas.GetError();
        const auto rows = VFX::BuildEmitterParams( cb.GetValue(), sb, 0, atlas.GetValue() );
        ASSERT_TRUE( rows.IsSuccess() ) << rows.GetError();
        const auto& r = rows.GetValue();
        EXPECT_NE( std::find( r.begin(), r.end(), glm::vec4( 777.0f, 0.0f, 0.0f, 0.0f ) ), r.end() )
             << "StartSpeed";
        EXPECT_NE( std::find( r.begin(), r.end(), glm::vec4( 10.0f, 20.0f, 30.0f, 0.0f ) ), r.end() )
             << "the emitter position";
        EXPECT_NE( std::find( r.begin(), r.end(), glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ) ), r.end() ) << "LocalSpace";
    }
} // namespace
