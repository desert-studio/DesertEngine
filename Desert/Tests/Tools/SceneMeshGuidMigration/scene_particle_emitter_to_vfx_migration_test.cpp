// VFX-03: a ParticleEmitter block becomes a `.dfx` system of one emitter whose module stack reproduces it, and the
// block becomes a VFX block naming that file (SceneMigration.hpp, MigrateParticleEmittersToVFX).

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
namespace S         = Desert::Assets::Serialization;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabOverrideData;

namespace
{
    rfl::Generic Vec3( float x, float y, float z )
    {
        return rfl::Generic( std::vector<rfl::Generic>{ rfl::Generic( static_cast<double>( x ) ),
                                                        rfl::Generic( static_cast<double>( y ) ),
                                                        rfl::Generic( static_cast<double>( z ) ) } );
    }

    rfl::Generic::Object Block( double spawnRate )
    {
        rfl::Generic::Object b;
        b["SpawnRate"]        = rfl::Generic( spawnRate );
        b["MaxParticles"]     = rfl::Generic( static_cast<int64_t>( 300 ) );
        b["Lifetime"]         = rfl::Generic( 2.0 );
        b["LifetimeVariance"] = rfl::Generic( 0.5 );
        b["StartSpeed"]       = rfl::Generic( 100.0 );
        b["SpeedVariance"]    = rfl::Generic( 0.25 );
        b["ConeAngle"]        = rfl::Generic( 30.0 );
        b["Gravity"]          = Vec3( 0.0f, -980.0f, 0.0f );
        b["StartSize"]        = rfl::Generic( 10.0 );
        b["EndSize"]          = rfl::Generic( 2.0 );
        b["SizeCurvePower"]   = rfl::Generic( 2.0 );
        b["StartColor"]       = Vec3( 0.0f, 0.5f, 1.0f );
        b["EndColor"]         = Vec3( 1.0f, 0.0f, 0.0f );
        b["WorldSpace"]       = rfl::Generic( false );
        return b;
    }

    EntityData Emitter( uint64_t uuid, double spawnRate )
    {
        EntityData entity;
        entity.id                            = Desert::Common::UUID( uuid );
        entity.Tag                           = "Torch";
        entity.Components["ParticleEmitter"] = rfl::Generic( Block( spawnRate ) );
        return entity;
    }

    const S::VFXModuleUse& Find( const std::vector<S::VFXModuleUse>& group, const std::string& name )
    {
        for ( const auto& use : group )
            if ( use.Module == "engine:" + name )
                return use;
        ADD_FAILURE() << "no module " << name;
        return group.front();
    }

    const S::VFXModuleInput& In( const S::VFXModuleUse& use, const std::string& name )
    {
        for ( const auto& in : use.Inputs )
            if ( in.Name == name )
                return in;
        ADD_FAILURE() << use.Module << " has no input " << name;
        return use.Inputs.front();
    }
} // namespace

// Red if any old parameter is lost or lands on the wrong module/input, or the random ranges stop matching the old
// shader's speed = S * (1 - v * r) / life = L * (1 - w * r).
TEST( ParticleEmitterToVFX, TheEmitterBecomesAStackWithEqualParameters )
{
    const auto read = Migration::ReadParticleEmitterV43( Block( 120.0 ) );
    ASSERT_TRUE( read ) << read.GetError();
    const S::VFXSystemData system = Migration::VFXSystemFromParticleEmitter( read.GetValue() );
    ASSERT_EQ( system.Emitters.size(), 1u );
    const S::VFXEmitterData& e = system.Emitters[0];

    EXPECT_EQ( system.Seed, 0u ); // VFXWorld's seed (0, uuid, 0) = the old component's
    EXPECT_EQ( system.Category, Migration::kVFXConvertedCategory );
    EXPECT_EQ( e.Capacity, 300u );
    EXPECT_EQ( e.Space, S::VFXSimulationSpace::Local );
    EXPECT_FLOAT_EQ( In( Find( e.Stack.EmitterUpdate, "SpawnRate" ), "SpawnRate" ).Value->x, 120.0f );

    const auto& life = In( Find( e.Stack.ParticleSpawn, "InitializeLifetime" ), "Lifetime" );
    EXPECT_FLOAT_EQ( life.Random->Min.x, 1.0f );
    EXPECT_FLOAT_EQ( life.Random->Max.x, 2.0f );
    const auto& cone = Find( e.Stack.ParticleSpawn, "AddVelocityInCone" );
    EXPECT_FLOAT_EQ( In( cone, "Speed" ).Random->Min.x, 75.0f );
    EXPECT_FLOAT_EQ( In( cone, "Speed" ).Random->Max.x, 100.0f );
    EXPECT_FLOAT_EQ( In( cone, "Angle" ).Value->x, 30.0f );
    EXPECT_FLOAT_EQ( In( cone, "Axis" ).Value->y, 1.0f );
    EXPECT_FLOAT_EQ( In( Find( e.Stack.ParticleUpdate, "Gravity" ), "Gravity" ).Value->y, -980.0f );

    // Size: Start + (End - Start) * t^2 at every key; colour: Start -> End per channel, the zero start included.
    const auto& size = *In( Find( e.Stack.ParticleUpdate, "SizeOverLife" ), "Scale" ).Curve;
    ASSERT_EQ( size.size(), 2u );
    ASSERT_EQ( size[0].size(), static_cast<std::size_t>( Migration::kVFXConvertedSizeSegments + 1 ) );
    for ( const auto& key : size[0] )
        EXPECT_NEAR( key.Value, 10.0f - 8.0f * key.Time * key.Time, 1e-4f );
    const auto& colour = *In( Find( e.Stack.ParticleUpdate, "ColorOverLife" ), "Scale" ).Curve;
    ASSERT_EQ( colour.size(), 4u );
    EXPECT_FLOAT_EQ( colour[0].front().Value, 0.0f );
    EXPECT_FLOAT_EQ( colour[0].back().Value, 1.0f );
    EXPECT_FLOAT_EQ( colour[3].back().Value, 0.0f ); // EndAlpha default

    // The generated system is one the `.dfx` validator accepts.
    EXPECT_TRUE( S::ValidateVFXSystemData( system ) );
}

// Red if the name or GUID depends on anything but the owner and the entity, a rerun writes other bytes, or the
// block is not replaced by a VFX block naming the file.
TEST( ParticleEmitterToVFX, NameAndGuidAreDeterministicAndARerunWritesTheSameFile )
{
    std::vector<EntityData> a{ Emitter( 0x1234, 50.0 ) };
    std::vector<EntityData> b{ Emitter( 0x1234, 50.0 ) };
    const auto              ra = Migration::MigrateParticleEmittersToVFX( a, "Level", "/assets" );
    const auto              rb = Migration::MigrateParticleEmittersToVFX( b, "Level", "/assets" );
    ASSERT_TRUE( ra.Refused.empty() );
    ASSERT_EQ( ra.NewSystems.size(), 1u );
    EXPECT_EQ( ra.NewSystems, rb.NewSystems );
    EXPECT_FALSE( a[0].Components.get( "ParticleEmitter" ).has_value() );
    const auto vfx = a[0].Components.get( "VFX" ).value().to_object().value();
    const auto ref = vfx.get( "System" ).value().to_object().value();
    EXPECT_EQ( ref.get( "Path" ).value().to_string().value(),
               "VFX/Level_" + Desert::Common::UUID( 0x1234 ).ToString() + ".dfx" );
    EXPECT_EQ( ref.get( "Guid" ).value().to_string().value(),
               Desert::Common::Content::AssetGuidToText(
                    Migration::MigrationGuidForPath( ref.get( "Path" ).value().to_string().value() ) ) );
    EXPECT_TRUE( vfx.get( "AutoActivate" ).value().to_bool().value() );

    // Rerun over the raised file: nothing left to convert.
    const auto again = Migration::MigrateParticleEmittersToVFX( a, "Level", "/assets" );
    EXPECT_EQ( again.Emitters, 0u );
    EXPECT_TRUE( again.NewSystems.empty() );
}

// Red if identical emitters stop sharing one file, or different ones share.
TEST( ParticleEmitterToVFX, IdenticalEmittersShareOneSystem )
{
    std::vector<EntityData> entities{ Emitter( 1, 50.0 ), Emitter( 2, 50.0 ), Emitter( 3, 70.0 ) };
    const auto              report = Migration::MigrateParticleEmittersToVFX( entities, "Level", "/assets" );
    EXPECT_EQ( report.Emitters, 3u );
    EXPECT_EQ( report.Shared, 1u );
    EXPECT_EQ( report.NewSystems.size(), 2u );
    const auto path = []( const EntityData& e )
    {
        return e.Components.get( "VFX" )
             .value()
             .to_object()
             .value()
             .get( "System" )
             .value()
             .to_object()
             .value()
             .get( "Path" );
    };
    EXPECT_EQ( path( entities[0] ).value().to_string(), path( entities[1] ).value().to_string() );
    EXPECT_NE( path( entities[0] ).value().to_string(), path( entities[2] ).value().to_string() );
}

// Red if a prefab override's partial ParticleEmitter block is converted on its own (it would lose the prefab's
// numbers) instead of refusing the file by name.
TEST( ParticleEmitterToVFX, APrefabOverrideOfTheEmitterRefusesTheFile )
{
    EntityData instance;
    instance.id = Desert::Common::UUID( 9 );
    PrefabOverrideData   override_;
    rfl::Generic::Object partial;
    partial["SpawnRate"]                    = rfl::Generic( 5.0 );
    override_.Components["ParticleEmitter"] = rfl::Generic( partial );
    instance.PrefabOverrides                = std::vector<PrefabOverrideData>{ override_ };
    std::vector<EntityData> entities{ instance };
    const auto              report = Migration::MigrateParticleEmittersToVFX( entities, "Level", "/assets" );
    ASSERT_EQ( report.Refused.size(), 1u );
    EXPECT_NE( report.Refused[0].find( "prefab override" ), std::string::npos );
}
