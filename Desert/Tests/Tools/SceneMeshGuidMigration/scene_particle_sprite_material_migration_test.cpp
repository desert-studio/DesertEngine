// VFX-08: a particle sprite composites by the blend mode of its MATERIAL (UE BLEND_Additive), so the emitter's own
// ParticleEmitter.Blend leaves the scene format and an Additive emitter is moved onto the shipped additive
// particle material (SceneMigration.hpp, kSceneVersionParticleSpriteMaterial).

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabOverrideData;

namespace
{
    rfl::Generic::Object EmitterOf( const rfl::ExtraFields<rfl::Generic>& components )
    {
        return components.get( "ParticleEmitter" ).value().to_object().value();
    }

    // An emitter block; `blend` unset = the key is not stated (the old default, AlphaBlend).
    rfl::Generic::Object EmitterBlock( std::optional<int> blend )
    {
        rfl::Generic::Object block;
        block["SpawnRate"] = rfl::Generic( 600.0 );
        if ( blend )
            block["Blend"] = rfl::Generic( static_cast<int64_t>( *blend ) );
        return block;
    }

    EntityData Emitter( const char* tag, std::optional<int> blend )
    {
        EntityData entity;
        entity.Tag                           = tag;
        entity.Components["ParticleEmitter"] = rfl::Generic( EmitterBlock( blend ) );
        return entity;
    }

    // The Guid of the material the block names, or nullopt when it names none.
    std::optional<std::string> MaterialGuid( const rfl::Generic::Object& block )
    {
        const auto material = block.get( "Material" );
        if ( !material.has_value() )
            return std::nullopt;
        return material.value().to_object().value().get( "Guid" ).value().to_string().value();
    }
} // namespace

// Red when the step is not the newest generation the engine requires.
TEST( SceneParticleSpriteMaterialMigration, VersionIsTheGenerationAfterCollisionProfiles )
{
    EXPECT_EQ( Migration::kSceneVersionParticleSpriteMaterial, Migration::kSceneVersionCollisionProfiles + 1 );
    EXPECT_EQ( Migration::kSceneVersionParticleSpriteMaterial, Desert::Core::kSceneVersion );
}

// Red when an Additive emitter keeps compositing over (no material), an AlphaBlend / unstated one gains a
// material, or any Blend key survives (the component no longer declares it).
TEST( SceneParticleSpriteMaterialMigration, AdditiveEmittersMoveOntoTheAdditiveMaterialAndBlendLeaves )
{
    std::vector<EntityData> entities = { Emitter( "Fire", 0 ), Emitter( "Smoke", 1 ),
                                         Emitter( "Dust", std::nullopt ) };
    const auto              report   = Migration::MigrateParticleSpriteMaterialsV42ToV43( entities );

    EXPECT_EQ( report.Emitters, 3u );
    EXPECT_EQ( report.MovedToAdditive, 1u );
    EXPECT_EQ( MaterialGuid( EmitterOf( entities[0].Components ) ),
               std::optional<std::string>( Migration::kParticleAdditiveMaterialGuid ) );
    EXPECT_EQ( EmitterOf( entities[0].Components )
                    .get( "Material" )
                    .value()
                    .to_object()
                    .value()
                    .get( "Path" )
                    .value()
                    .to_string()
                    .value(),
               std::string( Migration::kParticleAdditiveMaterialPath ) );
    EXPECT_FALSE( MaterialGuid( EmitterOf( entities[1].Components ) ).has_value() )
         << "an AlphaBlend emitter draws with the default translucent sprite material: no material stated";
    EXPECT_FALSE( MaterialGuid( EmitterOf( entities[2].Components ) ).has_value() );
    for ( const auto& entity : entities )
        EXPECT_FALSE( EmitterOf( entity.Components ).get( "Blend" ).has_value() ) << entity.Tag.value_or( "" );
}

// Red when an emitter that already names a material has it replaced, or a prefab override's Blend survives
// / an Additive override is not moved.
TEST( SceneParticleSpriteMaterialMigration, AStatedMaterialIsKeptAndOverridesMoveAlike )
{
    EntityData           named = Emitter( "Named", 0 );
    rfl::Generic::Object block = EmitterOf( named.Components );
    rfl::Generic::Object own;
    own["Guid"]                         = rfl::Generic( std::string( "00112233445566778899aabbccddeeff" ) );
    own["Path"]                         = rfl::Generic( std::string( "assets:Materials/M_Embers.demat" ) );
    block["Material"]                   = rfl::Generic( std::move( own ) );
    named.Components["ParticleEmitter"] = rfl::Generic( std::move( block ) );

    EntityData         instance;
    PrefabOverrideData additive;
    additive.Components["ParticleEmitter"] = rfl::Generic( EmitterBlock( 0 ) );
    PrefabOverrideData over;
    over.Components["ParticleEmitter"] = rfl::Generic( EmitterBlock( 1 ) );
    instance.PrefabOverrides           = std::vector<PrefabOverrideData>{ additive, over };

    std::vector<EntityData> entities = { named, instance };
    const auto              report   = Migration::MigrateParticleSpriteMaterialsV42ToV43( entities );

    EXPECT_EQ( MaterialGuid( EmitterOf( entities[0].Components ) ),
               std::optional<std::string>( "00112233445566778899aabbccddeeff" ) );
    EXPECT_EQ( report.MovedToAdditive, 0u );
    EXPECT_EQ( report.OverridesAdditive, 1u );
    EXPECT_EQ( report.OverridesDropped, 1u );
    const auto& overrides = *entities[1].PrefabOverrides;
    EXPECT_EQ( MaterialGuid( EmitterOf( overrides[0].Components ) ),
               std::optional<std::string>( Migration::kParticleAdditiveMaterialGuid ) );
    EXPECT_FALSE( EmitterOf( overrides[0].Components ).get( "Blend" ).has_value() );
    EXPECT_FALSE( EmitterOf( overrides[1].Components ).get( "Blend" ).has_value() );
    EXPECT_FALSE( MaterialGuid( EmitterOf( overrides[1].Components ) ).has_value() );
}
