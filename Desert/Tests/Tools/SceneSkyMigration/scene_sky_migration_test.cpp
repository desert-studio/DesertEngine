// The sky schema migration, v0 -> v1, checked on the only thing a machine without a GPU can check
// honestly: the data.
//
// The migration is the one place in this programme where "it looks right" is not available as evidence.
// It is arithmetic over a parsed JSON tree, so every rule in the specification - what is carried, what is
// converted, what is rejected, what is left alone - is a value comparison, and a scene that silently loses
// its sky on load is exactly the failure these cases exist to catch.
//
// The migration writes a payload; the reflection serializer is what turns that payload into the component
// the engine actually runs with. So the assertions are made where it matters - on the loaded
// SkyAtmosphereData - by compiling the generated reflection table straight into this binary, the same way
// ComponentReflection does. No renderer, no scene, no asset manager.

#include <SceneMigration.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::ECS::SkyAtmosphereData;
using Desert::ECS::SkyPreset;
using Desert::Migration::kSkyAtmosphereFieldCount;
using Desert::Migration::MigrateSkyV0ToV1;
using Desert::Migration::SkyMigrationReport;
using Desert::Reflection::ReflectionRegistry;

namespace
{
    // The "Skybox" payload of Editor/Resources/Assets/Scenes/Desert_Sandbox.desce, copied out of the file
    // verbatim - including the exact doubles, which are what a float32 round trip produces. This is the
    // fixture of T1: the real scene the engine has been shipping, not a hand-written approximation of it.
    rfl::Generic::Object RealSandboxSkybox()
    {
        rfl::Generic::Object o;
        o["SkyboxHandle"]  = std::string( "" );
        o["Intensity"]     = 1.0;
        o["Procedural"]    = true;
        o["SunIntensity"]  = 22.0;
        o["SunDiskRadius"] = 0.019999999552965164;
        o["ZenithColor"]   = rfl::Generic::Array{ 0.07999999821186066, 0.25999999046325684, 0.699999988079071 };
        o["HorizonColor"]  = rfl::Generic::Array{ 0.5, 0.6600000262260437, 0.9200000166893005 };
        o["GroundColor"]   = rfl::Generic::Array{ 0.1599999964237213, 0.1899999976158142, 0.23999999463558197 };
        o["NightColor"] = rfl::Generic::Array{ 0.009999999776482582, 0.019999999552965164, 0.05000000074505806 };
        o["SkyBrightness"]   = 1.0;
        o["HorizonFalloff"]  = 0.8500000238418579;
        o["SunColor"]        = rfl::Generic::Array{ 1.0, 0.9599999785423279, 0.8799999952316284 };
        o["SunGlow"]         = 1.0;
        o["SunsetColor"]     = rfl::Generic::Array{ 1.0, 0.41999998688697815, 0.18000000715255737 };
        o["SunsetIntensity"] = 1.0;
        o["StarIntensity"]   = 1.0;
        o["EnableClouds"]    = false;
        o["CloudCoverage"]   = 0.5;
        o["CloudDensity"]    = 1.0;
        o["CloudTiling"]     = 1.5;
        o["CloudBrightness"] = 1.0;
        o["CloudWindSpeed"]  = 8.0;
        return o;
    }

    EntityData MakeEntity( const char* tag )
    {
        EntityData e;
        e.Tag = std::string( tag );
        return e;
    }

    EntityData WithSkybox( const char* tag, rfl::Generic::Object payload )
    {
        EntityData e           = MakeEntity( tag );
        e.Components["Skybox"] = rfl::Generic( std::move( payload ) );
        return e;
    }

    bool HasPayload( const EntityData& e, const char* key )
    {
        return e.Components.get( key ).has_value();
    }

    rfl::Generic::Object PayloadOf( const EntityData& e, const char* key )
    {
        const auto found = e.Components.get( key );
        EXPECT_TRUE( found.has_value() ) << "entity carries no '" << key << "' payload";
        if ( !found.has_value() )
            return {};
        return found.value().to_object().value_or( rfl::Generic::Object{} );
    }

    // The component the ENGINE would end up with: the migrated payload put through the very function
    // ComponentRegistry's reflected serializer calls. Asserting here rather than on the raw JSON is what
    // makes "a rejected field keeps its default" a statement about the running engine and not about a map.
    SkyAtmosphereData LoadSky( const EntityData& e )
    {
        SkyAtmosphereData data;
        const auto*       type = ReflectionRegistry::Get().Find( "SkyAtmosphereData" );
        EXPECT_NE( type, nullptr );
        if ( !type )
            return data;
        Desert::Reflection::DeserializeReflected( *type, &data, PayloadOf( e, "SkyAtmosphere" ) );
        return data;
    }

    std::string TreeToJson( const std::vector<EntityData>& entities )
    {
        return rfl::json::write( entities );
    }

    // 12 of the 14 mappings are same-name copies (SKY-05 kept the C++ member names byte-identical across
    // the move precisely so that they would be). Naming them here means a mapping that quietly stopped
    // copying shows up as a named failure rather than as a wrong picture.
    constexpr const char* kSameNamedScalars[] = { "SkyBrightness", "HorizonFalloff",  "SunIntensity",
                                                  "SunGlow",       "SunsetIntensity", "StarIntensity" };
    constexpr const char* kSameNamedColours[] = { "ZenithColor", "HorizonColor", "GroundColor",
                                                  "NightColor",  "SunColor",     "SunsetColor" };
} // namespace

// ---------------------------------------------------------------------------------------------------
// T1 - the real shipped payload, verbatim.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, RealSandboxPayloadCarriesEveryValue )
{
    std::vector<EntityData> entities{ WithSkybox( "Sky", RealSandboxSkybox() ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.FieldsCarried, 14 );
    EXPECT_EQ( report.FieldsDefaulted, kSkyAtmosphereFieldCount - 14 );
    EXPECT_EQ( report.FieldsRejected, 0 );

    const SkyAtmosphereData sky = LoadSky( entities[0] );

    // The old Procedural flag becomes Enabled - the component exists either way, the flag says whether the
    // atmosphere renders.
    EXPECT_TRUE( sky.Enabled );

    // The twelve same-named values, exactly as authored. float32 comparison is exact on purpose: these are
    // copies, and a copy that changes a value is a bug, not a rounding difference.
    EXPECT_FLOAT_EQ( sky.SkyBrightness, 1.0f );
    EXPECT_FLOAT_EQ( sky.HorizonFalloff, 0.85f );
    EXPECT_FLOAT_EQ( sky.SunIntensity, 22.0f );
    EXPECT_FLOAT_EQ( sky.SunGlow, 1.0f );
    EXPECT_FLOAT_EQ( sky.SunsetIntensity, 1.0f );
    EXPECT_FLOAT_EQ( sky.StarIntensity, 1.0f );

    EXPECT_FLOAT_EQ( sky.ZenithColor.x, 0.08f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.y, 0.26f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.z, 0.70f );
    EXPECT_FLOAT_EQ( sky.HorizonColor.x, 0.50f );
    EXPECT_FLOAT_EQ( sky.HorizonColor.y, 0.66f );
    EXPECT_FLOAT_EQ( sky.HorizonColor.z, 0.92f );
    EXPECT_FLOAT_EQ( sky.GroundColor.x, 0.16f );
    EXPECT_FLOAT_EQ( sky.GroundColor.y, 0.19f );
    EXPECT_FLOAT_EQ( sky.GroundColor.z, 0.24f );
    EXPECT_FLOAT_EQ( sky.NightColor.x, 0.010f );
    EXPECT_FLOAT_EQ( sky.NightColor.y, 0.020f );
    EXPECT_FLOAT_EQ( sky.NightColor.z, 0.050f );
    EXPECT_FLOAT_EQ( sky.SunColor.x, 1.00f );
    EXPECT_FLOAT_EQ( sky.SunColor.y, 0.96f );
    EXPECT_FLOAT_EQ( sky.SunColor.z, 0.88f );
    EXPECT_FLOAT_EQ( sky.SunsetColor.x, 1.00f );
    EXPECT_FLOAT_EQ( sky.SunsetColor.y, 0.42f );
    EXPECT_FLOAT_EQ( sky.SunsetColor.z, 0.18f );

    // The one conversion: 0.02 rad of angular RADIUS is 2.2918 deg of angular DIAMETER. Dropping the x2
    // halves the sun and is invisible in a screenshot, which is why it is asserted to 1e-4.
    EXPECT_NEAR( sky.SunAngularDiameter, 2.2918f, 1e-4f );

    // A migrated palette was authored by hand, so claiming it came from a preset would be a lie the scene
    // then carries forever.
    EXPECT_EQ( sky.ActivePreset, SkyPreset::Custom );

    // Every mapped field really was written, by name - a table row that stopped firing would otherwise
    // hide behind the value happening to equal the C++ default.
    const rfl::Generic::Object payload = PayloadOf( entities[0], "SkyAtmosphere" );
    for ( const char* name : kSameNamedScalars )
        EXPECT_TRUE( payload.get( name ).has_value() ) << name << " was not written";
    for ( const char* name : kSameNamedColours )
        EXPECT_TRUE( payload.get( name ).has_value() ) << name << " was not written";
    EXPECT_TRUE( payload.get( "Enabled" ).has_value() );
    EXPECT_TRUE( payload.get( "SunAngularDiameter" ).has_value() );

    // Fields with nothing to carry are ABSENT from the payload rather than written as a second copy of the
    // component's defaults - one source of truth for a default value.
    EXPECT_FALSE( payload.get( "PlanetRadius" ).has_value() );
    EXPECT_FALSE( payload.get( "TimeOfDay" ).has_value() );
    EXPECT_FLOAT_EQ( sky.PlanetRadius, 6360.0f );
    EXPECT_FLOAT_EQ( sky.TimeOfDay, 12.0f );
}

// ---------------------------------------------------------------------------------------------------
// T2 - a payload with a single field. The counters have to add up to the component's field count.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, OnlyProceduralLeavesEverythingElseDefaulted )
{
    rfl::Generic::Object old;
    old["Procedural"] = true;

    std::vector<EntityData> entities{ WithSkybox( "Sky", std::move( old ) ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.FieldsCarried, 1 );
    EXPECT_EQ( report.FieldsDefaulted, kSkyAtmosphereFieldCount - 1 );
    EXPECT_EQ( report.FieldsRejected, 0 );
    EXPECT_EQ( report.FieldsCarried + report.FieldsDefaulted + report.FieldsRejected, kSkyAtmosphereFieldCount );

    const SkyAtmosphereData sky = LoadSky( entities[0] );
    EXPECT_TRUE( sky.Enabled );
    EXPECT_FLOAT_EQ( sky.SkyBrightness, 1.0f );
    EXPECT_NEAR( sky.SunAngularDiameter, 2.2918f, 1e-4f );
}

// ---------------------------------------------------------------------------------------------------
// T3 - values of the wrong JSON type. Rejected, counted, and the component keeps its defaults.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, WronglyTypedValuesAreRejectedNotGuessed )
{
    rfl::Generic::Object old;
    old["SkyBrightness"] = std::string( "bright" );
    old["ZenithColor"]   = std::string( "blue" );

    std::vector<EntityData> entities{ WithSkybox( "Sky", std::move( old ) ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.FieldsCarried, 0 );
    EXPECT_EQ( report.FieldsRejected, 2 );
    EXPECT_EQ( report.FieldsDefaulted, kSkyAtmosphereFieldCount - 2 );

    const SkyAtmosphereData sky = LoadSky( entities[0] );
    EXPECT_FLOAT_EQ( sky.SkyBrightness, 1.0f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.x, 0.08f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.y, 0.26f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.z, 0.70f );
}

// ---------------------------------------------------------------------------------------------------
// T4 - idempotence. A second run must be a no-op, byte for byte.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, SecondRunChangesNothing )
{
    std::vector<EntityData> entities{ WithSkybox( "Sky", RealSandboxSkybox() ) };

    const SkyMigrationReport first = MigrateSkyV0ToV1( entities );
    EXPECT_EQ( first.Entities, 1 );

    const std::string afterFirst = TreeToJson( entities );

    const SkyMigrationReport second = MigrateSkyV0ToV1( entities );
    EXPECT_EQ( second.Entities, 0 );
    EXPECT_EQ( second.FieldsCarried, 0 );
    EXPECT_EQ( second.FieldsDefaulted, 0 );
    EXPECT_EQ( second.FieldsRejected, 0 );

    EXPECT_EQ( TreeToJson( entities ), afterFirst );
}

// ---------------------------------------------------------------------------------------------------
// T5 - an entity with no sky at all is not touched and not counted.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, EntityWithoutSkyboxIsUntouched )
{
    std::vector<EntityData> entities{ MakeEntity( "Cube" ) };
    entities[0].Components["StaticMesh"] = rfl::Generic( rfl::Generic::Object{} );

    const std::string before = TreeToJson( entities );

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.FieldsCarried, 0 );
    EXPECT_EQ( report.FieldsDefaulted, 0 );
    EXPECT_EQ( report.FieldsRejected, 0 );
    EXPECT_FALSE( HasPayload( entities[0], "SkyAtmosphere" ) );
    EXPECT_EQ( TreeToJson( entities ), before );
}

// ---------------------------------------------------------------------------------------------------
// T6 - a colour with the wrong number of components.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, ShortColourArrayIsRejected )
{
    rfl::Generic::Object old;
    old["ZenithColor"] = rfl::Generic::Array{ 0.1, 0.2 };

    std::vector<EntityData> entities{ WithSkybox( "Sky", std::move( old ) ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.FieldsRejected, 1 );
    EXPECT_EQ( report.FieldsCarried, 0 );

    // Not "the first two components applied and the third left over" - a partly-applied colour is worse
    // than a default one, because it looks deliberate.
    const SkyAtmosphereData sky = LoadSky( entities[0] );
    EXPECT_FLOAT_EQ( sky.ZenithColor.x, 0.08f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.y, 0.26f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.z, 0.70f );
}

TEST( SceneSkyMigration, NonFiniteColourComponentIsRejected )
{
    rfl::Generic::Object old;
    old["HorizonColor"] = rfl::Generic::Array{ 0.1, std::numeric_limits<double>::quiet_NaN(), 0.3 };

    std::vector<EntityData> entities{ WithSkybox( "Sky", std::move( old ) ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.FieldsRejected, 1 );

    const SkyAtmosphereData sky = LoadSky( entities[0] );
    EXPECT_FLOAT_EQ( sky.HorizonColor.x, 0.50f );
    EXPECT_TRUE( std::isfinite( sky.HorizonColor.y ) );
}

// ---------------------------------------------------------------------------------------------------
// T7 - a sun disk that never was one.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, ZeroSunDiskRadiusIsRejected )
{
    rfl::Generic::Object old;
    old["SunDiskRadius"] = 0.0;

    std::vector<EntityData> entities{ WithSkybox( "Sky", std::move( old ) ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.FieldsRejected, 1 );
    EXPECT_EQ( report.FieldsCarried, 0 );
    EXPECT_NEAR( LoadSky( entities[0] ).SunAngularDiameter, 2.2918f, 1e-4f );
}

TEST( SceneSkyMigration, NonFiniteSunDiskRadiusIsRejected )
{
    rfl::Generic::Object old;
    old["SunDiskRadius"] = std::numeric_limits<double>::quiet_NaN();

    std::vector<EntityData> entities{ WithSkybox( "Sky", std::move( old ) ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.FieldsRejected, 1 );
    EXPECT_NEAR( LoadSky( entities[0] ).SunAngularDiameter, 2.2918f, 1e-4f );
}

// ---------------------------------------------------------------------------------------------------
// WHAT A FILE FROM THE PAST MAY CARRY THAT THE ENGINE NO LONGER KNOWS.
//
// A .desce written by an older build holds keys for fields and components that have since been removed,
// and a scene in the wild must still load. Both halves of the loader are index-driven rather than
// file-driven — DeserializeReflected walks the TYPE's fields and looks each up in the file, and
// EntitySerializer walks the component REGISTRY and looks each key up in the entity — so a key nobody
// claims is simply never visited. It is not an error, it is not a warning, and it disappears the next
// time the scene is saved, because a save writes only what the registry knows.
//
// The two tests below pin that at both levels, using the keys a real pre-split Sandbox file carries.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, UnknownFieldsOfAKnownPayloadAreNeitherReadNorRemoved )
{
    std::vector<EntityData> entities{ WithSkybox( "Sky", RealSandboxSkybox() ) };

    MigrateSkyV0ToV1( entities );

    // Six keys of a feature the engine no longer has. The migration maps what it maps and leaves the
    // rest of the payload byte for byte as it found it.
    const rfl::Generic::Object skybox = PayloadOf( entities[0], "Skybox" );
    EXPECT_TRUE( skybox.get( "EnableClouds" ).has_value() );
    EXPECT_TRUE( skybox.get( "CloudCoverage" ).has_value() );
    EXPECT_TRUE( skybox.get( "CloudDensity" ).has_value() );
    EXPECT_TRUE( skybox.get( "CloudTiling" ).has_value() );
    EXPECT_TRUE( skybox.get( "CloudBrightness" ).has_value() );
    EXPECT_TRUE( skybox.get( "CloudWindSpeed" ).has_value() );

    // ...and none of them leaked into the payload the migration wrote.
    const rfl::Generic::Object sky = PayloadOf( entities[0], "SkyAtmosphere" );
    EXPECT_FALSE( sky.get( "EnableClouds" ).has_value() );
    EXPECT_FALSE( sky.get( "CloudCoverage" ).has_value() );

    // The HDR path stays where it is, too.
    EXPECT_TRUE( skybox.get( "SkyboxHandle" ).has_value() );
    EXPECT_TRUE( skybox.get( "Intensity" ).has_value() );
}

TEST( SceneSkyMigration, AComponentBlockNoRegistryClaimsIsSkippedRatherThanRejected )
{
    // The COMPONENT level of the same rule, and the one that decides whether removing a component
    // breaks every saved scene: a payload keyed by a component that no longer exists must survive the
    // migration untouched and be deserialized by nobody.
    std::vector<EntityData> entities{ WithSkybox( "Sky", RealSandboxSkybox() ) };
    rfl::Generic::Object    orphan;
    orphan["Enabled"]                                     = true;
    orphan["Coverage"]                                    = 0.5;
    entities[0].Components["ComponentThatNoLongerExists"] = rfl::Generic( orphan );

    MigrateSkyV0ToV1( entities );

    // Still there and still ignored: the migration iterates the keys it knows, so it neither reads nor
    // removes this one, and no registered component will ever look it up.
    ASSERT_TRUE( HasPayload( entities[0], "ComponentThatNoLongerExists" ) );
    EXPECT_TRUE( PayloadOf( entities[0], "ComponentThatNoLongerExists" ).get( "Enabled" ).has_value() );

    // And the entity it sits on migrates exactly as it would have without it.
    EXPECT_TRUE( HasPayload( entities[0], "SkyAtmosphere" ) );
}

// ---------------------------------------------------------------------------------------------------
// T9 - round trip. What the migration writes has to survive a load and a save unchanged, and the Skybox
// block that comes back out has to be the two-field component SkyboxComponent is now.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, RoundTripThroughTheReflectedSerializers )
{
    std::vector<EntityData> entities{ WithSkybox( "Sky", RealSandboxSkybox() ) };
    MigrateSkyV0ToV1( entities );

    const auto* skyType    = ReflectionRegistry::Get().Find( "SkyAtmosphereData" );
    const auto* skyboxType = ReflectionRegistry::Get().Find( "SkyboxComponent" );
    ASSERT_NE( skyType, nullptr );
    ASSERT_NE( skyboxType, nullptr );

    // Load exactly as ComponentRegistry's reflected serializers do, then save the same way.
    Desert::ECS::SkyAtmosphereComponent skyComponent;
    Desert::Reflection::DeserializeReflected( *skyType, &skyComponent.Data,
                                              PayloadOf( entities[0], "SkyAtmosphere" ) );

    Desert::ECS::SkyboxComponent skyboxComponent;
    Desert::Reflection::DeserializeReflected( *skyboxType, &skyboxComponent, PayloadOf( entities[0], "Skybox" ) );

    const rfl::Generic::Object savedSky = Desert::Reflection::SerializeReflected( *skyType, &skyComponent.Data );
    const rfl::Generic::Object savedSkybox =
         Desert::Reflection::SerializeReflected( *skyboxType, &skyboxComponent );

    EXPECT_EQ( static_cast<int>( savedSky.size() ), kSkyAtmosphereFieldCount );
    EXPECT_TRUE( savedSky.get( "ZenithColor" ).has_value() );
    EXPECT_NEAR( skyComponent.Data.SunAngularDiameter, 2.2918f, 1e-4f );

    // The saved sky survives a second load with the same values - the payload the migration produced is a
    // fixed point, not a shape that degrades each time the scene is opened.
    Desert::ECS::SkyAtmosphereData reloaded;
    Desert::Reflection::DeserializeReflected( *skyType, &reloaded, savedSky );
    EXPECT_FLOAT_EQ( reloaded.ZenithColor.x, skyComponent.Data.ZenithColor.x );
    EXPECT_FLOAT_EQ( reloaded.SunAngularDiameter, skyComponent.Data.SunAngularDiameter );
    EXPECT_EQ( reloaded.Enabled, skyComponent.Data.Enabled );

    // What a save writes for the Skybox component now: the cubemap and its authored look, and none of
    // the sky ones. The stale keys still sitting in the file are read by nobody and disappear on the
    // next save — which is the whole disposal route for a removed field, and why no migration is owed
    // for one.
    //
    // THE ROWS ARE PINNED AND THE COUNT IS DERIVED, which is the opposite of what stood here. A literal
    // `2u` is a gate that a new field turns red and that the next person satisfies by typing `4u` — the
    // number is not the property, the SET is. Adding Rotation and Tint (2026-09-23) turned it red for
    // exactly that reason and nothing else. Derived from the reflected type, the assertion still says
    // "a save writes these and nothing more", and it says it without a number anyone can edit.
    EXPECT_TRUE( savedSkybox.get( "SkyboxHandle" ).has_value() );
    EXPECT_TRUE( savedSkybox.get( "Intensity" ).has_value() );
    EXPECT_TRUE( savedSkybox.get( "Rotation" ).has_value() );
    EXPECT_TRUE( savedSkybox.get( "Tint" ).has_value() );
    EXPECT_FALSE( savedSkybox.get( "Procedural" ).has_value() );
    EXPECT_FALSE( savedSkybox.get( "ZenithColor" ).has_value() );
    EXPECT_FALSE( savedSkybox.get( "CloudCoverage" ).has_value() );
    EXPECT_EQ( savedSkybox.size(), skyboxType->Fields.size() )
         << "a save wrote a different number of keys than SkyboxComponent reflects — the serializer and "
            "the component disagree about what the component IS, which is a defect in one of them and "
            "not a number to adjust here";
}

// ---------------------------------------------------------------------------------------------------
// The field count the counters are arithmetic on is tied to the component itself, so growing the
// component without revisiting the migration fails here rather than skewing every report in the log.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, FieldCountMatchesTheReflectedComponent )
{
    const auto* type = ReflectionRegistry::Get().Find( "SkyAtmosphereData" );
    ASSERT_NE( type, nullptr );
    EXPECT_EQ( static_cast<int>( type->Fields.size() ), kSkyAtmosphereFieldCount );
}

// ---------------------------------------------------------------------------------------------------
// An HDR-mode scene authored a palette too. Creating the component only for procedural scenes would
// throw that palette away the first time someone switched the mode back.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, ComponentIsCreatedEvenWhenProceduralWasOff )
{
    rfl::Generic::Object old = RealSandboxSkybox();
    old["Procedural"]        = false;

    std::vector<EntityData> entities{ WithSkybox( "Sky", std::move( old ) ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.Entities, 1 );
    ASSERT_TRUE( HasPayload( entities[0], "SkyAtmosphere" ) );

    const SkyAtmosphereData sky = LoadSky( entities[0] );
    EXPECT_FALSE( sky.Enabled );
    EXPECT_FLOAT_EQ( sky.ZenithColor.z, 0.70f );
}

// ---------------------------------------------------------------------------------------------------
// A scene may hold several sky entities (multi-scene editing merges views). The counters sum; each
// entity is decided on its own.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, CountersSumAcrossEntitiesAndSkipMigratedOnes )
{
    rfl::Generic::Object partial;
    partial["Procedural"]    = true;
    partial["SkyBrightness"] = 2.5;

    std::vector<EntityData> entities{
         WithSkybox( "Sky", RealSandboxSkybox() ),
         MakeEntity( "Cube" ),
         WithSkybox( "SecondSky", std::move( partial ) ),
         WithSkybox( "AlreadyDone", RealSandboxSkybox() ),
    };
    entities[3].Components["SkyAtmosphere"] = rfl::Generic( rfl::Generic::Object{} );

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.Entities, 2 );
    EXPECT_EQ( report.FieldsCarried, 14 + 2 );
    EXPECT_EQ( report.FieldsRejected, 0 );
    EXPECT_EQ( report.FieldsDefaulted, 2 * kSkyAtmosphereFieldCount - 16 );

    EXPECT_FALSE( HasPayload( entities[1], "SkyAtmosphere" ) );
    EXPECT_FLOAT_EQ( LoadSky( entities[2] ).SkyBrightness, 2.5f );
    // The already-migrated entity kept its (empty) payload instead of being overwritten from stale data.
    EXPECT_EQ( PayloadOf( entities[3], "SkyAtmosphere" ).size(), 0u );
}

// ---------------------------------------------------------------------------------------------------
// A Skybox key that is not an object at all - a hand-edited or truncated file. It must not create a
// component out of nothing, and it must not take the loader down with it.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, MalformedSkyboxPayloadCreatesNothing )
{
    EntityData e           = MakeEntity( "Sky" );
    e.Components["Skybox"] = rfl::Generic( std::string( "procedural" ) );

    std::vector<EntityData> entities{ std::move( e ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.FieldsCarried, 0 );
    EXPECT_EQ( report.FieldsRejected, 0 );
    EXPECT_FALSE( HasPayload( entities[0], "SkyAtmosphere" ) );
}

// ---------------------------------------------------------------------------------------------------
// A hand-edited scene writes "SunIntensity":22, which JSON parses as an integer. Refusing it would reject
// a value that is perfectly legal and perfectly readable.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, IntegerSpelledNumbersAreAccepted )
{
    rfl::Generic::Object old;
    old["SunIntensity"] = static_cast<int64_t>( 22 );
    old["ZenithColor"]  = rfl::Generic::Array{ static_cast<int64_t>( 0 ), 0.5, static_cast<int64_t>( 1 ) };

    std::vector<EntityData> entities{ WithSkybox( "Sky", std::move( old ) ) };

    const SkyMigrationReport report = MigrateSkyV0ToV1( entities );

    EXPECT_EQ( report.FieldsCarried, 2 );
    EXPECT_EQ( report.FieldsRejected, 0 );

    const SkyAtmosphereData sky = LoadSky( entities[0] );
    EXPECT_FLOAT_EQ( sky.SunIntensity, 22.0f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.x, 0.0f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.y, 0.5f );
    EXPECT_FLOAT_EQ( sky.ZenithColor.z, 1.0f );
}

// ---------------------------------------------------------------------------------------------------
// The unit migration and this one run in the same load of an old scene. They are independent, and the
// evidence for that is that the sky values are identical whether or not the scene is metres-era.
// ---------------------------------------------------------------------------------------------------
TEST( SceneSkyMigration, IndependentOfTheUnitMigration )
{
    std::vector<EntityData> metresEra{ WithSkybox( "Sky", RealSandboxSkybox() ) };
    metresEra[0].Translation = glm::vec3( 1.0f, 2.0f, 3.0f );

    std::vector<EntityData> centimetreEra{ WithSkybox( "Sky", RealSandboxSkybox() ) };
    centimetreEra[0].Translation = glm::vec3( 100.0f, 200.0f, 300.0f );

    const SkyMigrationReport a = MigrateSkyV0ToV1( metresEra );
    const SkyMigrationReport b = MigrateSkyV0ToV1( centimetreEra );

    EXPECT_EQ( a.Entities, b.Entities );
    EXPECT_EQ( a.FieldsCarried, b.FieldsCarried );

    const SkyAtmosphereData first  = LoadSky( metresEra[0] );
    const SkyAtmosphereData second = LoadSky( centimetreEra[0] );
    EXPECT_FLOAT_EQ( first.SunAngularDiameter, second.SunAngularDiameter );
    EXPECT_FLOAT_EQ( first.SkyBrightness, second.SkyBrightness );
    EXPECT_FLOAT_EQ( first.PlanetRadius, second.PlanetRadius );

    // The transform is not this migration's business and stays exactly as it was found.
    EXPECT_FLOAT_EQ( metresEra[0].Translation->y, 2.0f );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
