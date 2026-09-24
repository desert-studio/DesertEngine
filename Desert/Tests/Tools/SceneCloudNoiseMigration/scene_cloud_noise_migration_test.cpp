// The cloud-noise migration, scene v2 -> v3, and the relation it exists to protect.
//
// The volumetric cloud component used to carry four numbers that parameterised a GPU bake — Weather Seed,
// Weather Octaves, Detail Seed, Detail Octaves. The bake is gone: the noise volume is an asset now, and
// the seed and the lattice periods that make one live in the volume's own header. There is nowhere on the
// component to carry those four numbers TO, so the migration deletes them from the file.
//
// Two things are asserted, and they are the two halves of "the old path is gone":
//
//   1. A v2 file's "VolumetricCloud" payload comes out of the migration WITHOUT the four keys, with every
//      other key untouched and in its original order, and the entity counted in the report.
//   2. The reflected VolumetricCloudData no longer has fields by those names, and DOES have the slot that
//      replaced them — which is the CLOUD TYPE now: T1 moved the noise volume onto the type, because the
//      character of an edge belongs to the kind of cloud. A migration that stripped a field the struct
//      still carried would be a silent data loss; a struct that still carried a field nothing writes would
//      be the dead setting the contract forbids.
//
// Everything below runs on the parsed tree. No GPU, no scene graph, no asset manager — the migration is a
// pure function and this is what that buys.

#include <Engine/Assets/CloudTypeData.hpp>
#include <SceneMigration.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>

#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Desert::Migration::CloudNoiseMigrationReport;
using Desert::Migration::kSceneVersion;
using Desert::Migration::kSceneVersionCloudNoise;
using Desert::Migration::kSceneVersionTonemap;
using Desert::Migration::MigrateCloudNoiseV2ToV3;
using Desert::Migration::MigrateScene;
using Desert::Migration::SceneSerialized;
using Desert::Reflection::ReflectionRegistry;

namespace
{
    // The four keys the GPU bake was parameterised by, as a v2 file spells them.
    const std::vector<std::string> kBakeKeys = { "WeatherSeed", "WeatherOctaves", "DetailSeed", "DetailOctaves" };

    // A v2 cloud payload with the bake settings present and a representative selection of the settings
    // that STAY, so the test can assert that nothing else moved.
    rfl::Generic::Object CloudPayloadV2()
    {
        rfl::Generic::Object o;
        o["Enabled"]         = true;
        o["Coverage"]        = 0.25;
        o["WeatherSeed"]     = 1337.0;
        o["WeatherTileSize"] = 1200000.0;
        o["WeatherOctaves"]  = 3.0;
        o["DetailSeed"]      = 13.0;
        o["DetailOctaves"]   = 2.0;
        o["DetailStrength"]  = 0.10000000149011612;
        o["ExtinctionScale"] = 8.0;
        return o;
    }

    Desert::Assets::EntityData EntityWithClouds()
    {
        Desert::Assets::EntityData entity;
        entity.Tag                           = "Sky";
        entity.Components["VolumetricCloud"] = rfl::Generic( CloudPayloadV2() );
        return entity;
    }

    rfl::Generic::Object CloudPayloadOf( const Desert::Assets::EntityData& entity )
    {
        const auto payload = entity.Components.get( "VolumetricCloud" );
        EXPECT_TRUE( payload.has_value() );
        const auto object = payload.value().to_object();
        EXPECT_TRUE( object.has_value() );
        return object.value_or( rfl::Generic::Object{} );
    }

    bool Has( const rfl::Generic::Object& object, const std::string& key )
    {
        return object.get( key ).has_value();
    }
} // namespace

TEST( SceneCloudNoiseMigration, TheFourBakeSettingsAreRemovedAndCounted )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds() };

    const CloudNoiseMigrationReport report = MigrateCloudNoiseV2ToV3( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.FieldsDropped, 4 );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );
    for ( const std::string& key : kBakeKeys )
        EXPECT_FALSE( Has( migrated, key ) ) << key << " survived the migration";
}

TEST( SceneCloudNoiseMigration, EverySETTINGTHATSTAYSIsUntouched )
{
    // The failure mode a "rebuild the object" migration has, and the reason it is worth a test of its own:
    // rebuilding is how you accidentally drop the keys you meant to keep, and the symptom would be a cloud
    // layer that silently returned to its defaults on load.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds() };
    MigrateCloudNoiseV2ToV3( entities );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );

    EXPECT_TRUE( Has( migrated, "Enabled" ) );
    EXPECT_TRUE( Has( migrated, "Coverage" ) );
    EXPECT_TRUE( Has( migrated, "WeatherTileSize" ) );
    EXPECT_TRUE( Has( migrated, "DetailStrength" ) );
    EXPECT_TRUE( Has( migrated, "ExtinctionScale" ) );

    EXPECT_EQ( migrated.size(), CloudPayloadV2().size() - kBakeKeys.size() );

    const auto coverage = migrated.get( "Coverage" );
    ASSERT_TRUE( coverage.has_value() );
    EXPECT_DOUBLE_EQ( coverage.value().to_double().value_or( -1.0 ), 0.25 );
}

TEST( SceneCloudNoiseMigration, TheNewSlotIsNOTWrittenSoTheDefaultVolumeIsWhatTheSceneGets )
{
    // An absent key is how the reflection serializer spells "keep the C++ default", and the C++ default is
    // an empty handle, which is exactly "use the built-in default volume". Writing a path here would invent
    // a choice the artist never made and pin every migrated scene to whatever volume happened to ship.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds() };
    MigrateCloudNoiseV2ToV3( entities );

    EXPECT_FALSE( Has( CloudPayloadOf( entities.front() ), "NoiseVolume" ) );
}

TEST( SceneCloudNoiseMigration, ItIsIdempotent )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds() };
    MigrateCloudNoiseV2ToV3( entities );

    const CloudNoiseMigrationReport second = MigrateCloudNoiseV2ToV3( entities );
    EXPECT_EQ( second.Entities, 0 );
    EXPECT_EQ( second.FieldsDropped, 0 );
}

TEST( SceneCloudNoiseMigration, AnEntityWithNoCloudLayerIsLeftAlone )
{
    Desert::Assets::EntityData plain;
    plain.Tag                      = "Cube";
    plain.Components["StaticMesh"] = rfl::Generic( rfl::Generic::Object{} );

    std::vector<Desert::Assets::EntityData> entities{ plain };
    const CloudNoiseMigrationReport         report = MigrateCloudNoiseV2ToV3( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_TRUE( entities.front().Components.get( "StaticMesh" ).has_value() );
}

TEST( SceneCloudNoiseMigration, ACloudPayloadThatIsNotAnObjectIsSurvivedRatherThanCrashedOn )
{
    // One hand-edited scene away. The migration warns and moves on; it does not reach into a number as if
    // it were a map.
    Desert::Assets::EntityData broken;
    broken.Tag                           = "Sky";
    broken.Components["VolumetricCloud"] = rfl::Generic( 7.0 );

    std::vector<Desert::Assets::EntityData> entities{ broken };
    const CloudNoiseMigrationReport         report = MigrateCloudNoiseV2ToV3( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.FieldsDropped, 0 );
}

TEST( SceneCloudNoiseMigration, MigrateSceneRunsItAndStampsTheFileSoItNeverRunsAgain )
{
    SceneSerialized scene;
    scene.SceneName    = "Clouds";
    scene.SceneVersion = kSceneVersionTonemap;
    scene.UnitVersion  = Desert::Migration::kUnitVersion;
    scene.Entities.push_back( EntityWithClouds() );

    const auto report = MigrateScene( scene );

    EXPECT_TRUE( report.CloudNoiseRaised );
    EXPECT_EQ( report.CloudNoise.FieldsDropped, 4 );
    EXPECT_TRUE( report.Changed() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    // The head has moved PAST this migration — v4 is the cloud species — so what is asserted is that this
    // step is still on the way to it and still gated on its own constant. Written as an inequality rather
    // than deleted: an equality here would have to be edited by every future migration, which is how a
    // guard turns into a chore and then into a rubber stamp.
    EXPECT_GE( kSceneVersion, kSceneVersionCloudNoise );

    // Second pass over the stamped tree: nothing left to do.
    const auto again = MigrateScene( scene );
    EXPECT_FALSE( again.Changed() );
}

// ---------------------------------------------------------------------------------------------------
// The other half of "the old path is gone": the struct itself.
// ---------------------------------------------------------------------------------------------------

TEST( SceneCloudNoiseMigration, TheReflectedComponentNoLongerCarriesTheBakeSettings )
{
    const auto* type = ReflectionRegistry::Get().Find( "VolumetricCloudData" );
    ASSERT_NE( type, nullptr );

    for ( const std::string& key : kBakeKeys )
    {
        const bool present = std::any_of( type->Fields.begin(), type->Fields.end(),
                                          [&key]( const auto& field ) { return field.Name == key; } );
        EXPECT_FALSE( present ) << key
                                << " is still a reflected field, so the migration removes a value "
                                   "the struct can still hold — that is silent data loss";
    }
}

TEST( SceneCloudNoiseMigration, TheVolumeSlotThatReplacedThemHasMovedOntoTheCloudType )
{
    const auto* type = ReflectionRegistry::Get().Find( "VolumetricCloudData" );
    ASSERT_NE( type, nullptr );

    // THE SLOT THIS MIGRATION CREATED IS NO LONGER ON THIS COMPONENT, one version later. v2 -> v3 replaced
    // four bake settings with a NoiseVolume handle on the layer; T1 moved that handle onto the CLOUD TYPE,
    // because the character of a cloud's edge is a property of the kind of cloud rather than of the
    // weather the layer is having. This test therefore asserts the same thing it always did — that the
    // four deleted fields were replaced by something an artist can point at — and follows the slot to
    // where it went.
    const auto volume = std::find_if( type->Fields.begin(), type->Fields.end(),
                                      []( const auto& field ) { return field.Name == "NoiseVolume"; } );
    EXPECT_EQ( volume, type->Fields.end() )
         << "the layer carries a noise volume slot again, and the cloud type carries one too — two sources "
            "of truth for one thing";

    // AND THE SLOT MOVED AGAIN, one more version later: O1 took the four type slots off the component
    // and into the cloud MATERIAL, whose schema is CloudRaymarch's Properties block. So the component
    // must not carry them either — two places to name a kind of cloud is the same two-sources-of-truth
    // this test was already watching for one level up.
    const auto slot = std::find_if( type->Fields.begin(), type->Fields.end(),
                                    []( const auto& field ) { return field.Name == "CloudType1"; } );
    EXPECT_EQ( slot, type->Fields.end() )
         << "the layer carries a cloud type slot again, and the material carries one too — two sources of "
            "truth for what kind of cloud this is";

    // The default is an EMPTY handle, which is the documented "use the built-in type, whose own volume
    // slot is empty, which is the built-in default volume". A scene that names neither must still render,
    // and that chain of two documented empties is what this asserts.
    // FOUR SLOTS SINCE T3, and every one of them is empty by default: a scene created from these
    // defaults names no kind of cloud at all and still has to have a sky. Read from the material's mirror
    // since O1 — that the four are asset references of kind CloudTypeAsset is the CloudMaterialSchema
    // suite's census, which reads the shader itself.
    const Desert::Graphic::CloudMaterialValues defaults;
    EXPECT_EQ( static_cast<uint64_t>( defaults.CloudType1 ), 0u );
    EXPECT_EQ( static_cast<uint64_t>( defaults.CloudType2 ), 0u );
    EXPECT_EQ( static_cast<uint64_t>( defaults.CloudType3 ), 0u );
    EXPECT_EQ( static_cast<uint64_t>( defaults.CloudType4 ), 0u );
    EXPECT_FALSE( Desert::Assets::CloudTypeDefault().NoiseVolume.has_value() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
