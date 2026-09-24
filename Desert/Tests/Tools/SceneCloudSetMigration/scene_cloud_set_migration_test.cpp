// The cloud-SET migration, scene v5 -> v6, and the relations it exists to protect.
//
// A cloud layer stopped carrying ONE kind of cloud and started carrying a set of up to four. One key moves
// and nothing else does: `CloudType` becomes `CloudType1`, the first of four slots.
//
// WHY THAT IS ENOUGH, AND WHY IT IS WORTH TESTING AT ALL. A rename looks like a migration with nothing in
// it, and the temptation is to skip both the function and its test by keeping the old name for the first
// slot. Three properties make it worth the function:
//
//   * THE SKY MUST NOT MOVE. The union of a one-element set is that element, and slot 0 takes the zero
//     decorrelation offset precisely so that a migrated layer reads the field T1 read, texel for texel.
//     A scene that went in with one kind of cloud comes out rendering the same sky, and this suite is what
//     says so about the FILE half of that claim.
//   * A HAND-EDITED FILE MUST NOT BE INTERPRETED. Whatever stands in the value — a path, an empty handle,
//     something a text editor put there — it meant "the kind of cloud this layer is made of" and it still
//     does. The migration moves the key and looks at nothing else, because validating here would be
//     answering a question the loader answers next, and two readers of one field are how they end up
//     disagreeing.
//   * IT MUST CHAIN. A v4 file has no `CloudType` key at all until the step before this one writes it, so
//     the two are ordered and the order is stated rather than inherited from the sequence they happen to
//     be written in.
//
// Everything below runs on the parsed tree. No GPU, no scene graph, no asset manager — the migration is a
// pure function and this is what that buys.

#include <Engine/Assets/CloudTypeData.hpp>
#include <SceneMigration.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using Desert::Migration::CloudSetMigrationReport;
using Desert::Migration::kSceneVersion;
using Desert::Migration::kSceneVersionCloudSet;
using Desert::Migration::kSceneVersionCloudType;
using Desert::Migration::MigrateCloudSetV5ToV6;
using Desert::Migration::MigrateScene;
using Desert::Migration::SceneSerialized;

namespace
{
    // A v5 cloud payload: the single cloud type slot plus a representative selection of the settings that
    // STAY, so the test can assert that nothing else moved.
    rfl::Generic::Object CloudPayloadV5( const std::string& typePath )
    {
        rfl::Generic::Object o;
        o["Enabled"]         = true;
        o["Coverage"]        = 0.25;
        o["CloudType"]       = typePath;
        o["WeatherTileSize"] = 1200000.0;
        o["DetailStrength"]  = 0.10000000149011612;
        o["ExtinctionScale"] = 8.0;
        return o;
    }

    Desert::Assets::EntityData EntityWithClouds( const std::string& typePath )
    {
        Desert::Assets::EntityData entity;
        entity.Tag                           = "Sky";
        entity.Components["VolumetricCloud"] = rfl::Generic( CloudPayloadV5( typePath ) );
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

    std::string StringAt( const rfl::Generic::Object& object, const std::string& key )
    {
        const auto value = object.get( key );
        if ( !value.has_value() )
            return {};
        return value.value().to_string().value_or( std::string{} );
    }

    // The names of the four slots, in the order the component declares them. Written out rather than
    // generated, because the FIRST of them is the one the migration writes and the other three are the
    // ones it must leave absent.
    constexpr const char* kSlots[] = { "CloudType1", "CloudType2", "CloudType3", "CloudType4" };
} // namespace

TEST( SceneCloudSetMigration, TheSingleTypeBecomesTheFirstSlotAndNothingElseIsInvented )
{
    const std::string congestus =
         Desert::Assets::CloudTypeAssetRelativePath( Desert::Assets::kCloudTypeCumulusCongestus );

    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( congestus ) };

    const CloudSetMigrationReport report = MigrateCloudSetV5ToV6( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.SlotsCarried, 1 );
    EXPECT_EQ( report.SlotsEmpty, 0 );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );

    EXPECT_FALSE( Has( migrated, "CloudType" ) ) << "the single slot survived the migration";
    EXPECT_EQ( StringAt( migrated, "CloudType1" ), congestus );

    // THE OTHER THREE ARE ABSENT, NOT EMPTY-STRINGED. An absent key is how the reflected serializer spells
    // "keep the C++ default", and the C++ default is the empty handle — which is the documented "this slot
    // holds no kind of cloud". Writing three empty values would be three statements of a default that the
    // struct already makes, and the first schema change to the default would leave them behind.
    for ( int slot = 1; slot < 4; ++slot )
        EXPECT_FALSE( Has( migrated, kSlots[slot] ) ) << kSlots[slot] << " was invented by the migration";
}

TEST( SceneCloudSetMigration, TheSlotKeepsItsPositionInTheFile )
{
    // The payload is rebuilt key by key, which is how you accidentally reorder a file. It matters less
    // than losing a key and it matters: a diff of a re-saved scene that shuffles every line is a diff
    // nobody reads, and this repository re-saves every scene whenever a migration lands.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( "Clouds/Types/Stratus.decloudtype" ) };
    MigrateCloudSetV5ToV6( entities );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );

    std::vector<std::string> keys;
    for ( const auto& [key, value] : migrated )
        keys.push_back( key );

    const std::vector<std::string> expected = { "Enabled",         "Coverage",       "CloudType1",
                                                "WeatherTileSize", "DetailStrength", "ExtinctionScale" };
    EXPECT_EQ( keys, expected ) << "the slot did not land where the single type stood";
}

TEST( SceneCloudSetMigration, EverySettingThatStaysIsUntouched )
{
    // The failure mode a "rebuild the object" migration has, and the reason it is worth a test of its own:
    // rebuilding is how you accidentally drop the keys you meant to keep, and the symptom would be a cloud
    // layer that silently returned to its defaults on load.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( "Clouds/Types/Cirrus.decloudtype" ) };
    MigrateCloudSetV5ToV6( entities );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );

    EXPECT_EQ( migrated.get( "Enabled" ).value().to_bool().value_or( false ), true );
    EXPECT_DOUBLE_EQ( migrated.get( "Coverage" ).value().to_double().value_or( 0.0 ), 0.25 );
    EXPECT_DOUBLE_EQ( migrated.get( "WeatherTileSize" ).value().to_double().value_or( 0.0 ), 1200000.0 );
    EXPECT_DOUBLE_EQ( migrated.get( "ExtinctionScale" ).value().to_double().value_or( 0.0 ), 8.0 );
}

TEST( SceneCloudSetMigration, AnEmptySlotIsCarriedAndCountedRatherThanDropped )
{
    // A layer that named no type is the state every scene in this repository is in, and it is a MEANING
    // rather than an absence: the built-in cumulus congestus. Dropping the key would produce the same
    // result today and would stop doing so the moment the default changed, so the value is carried and the
    // count says how many of them there were — the log line that reports this migration prints it.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( "" ) };

    const CloudSetMigrationReport report = MigrateCloudSetV5ToV6( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.SlotsCarried, 1 );
    EXPECT_EQ( report.SlotsEmpty, 1 );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );
    EXPECT_TRUE( Has( migrated, "CloudType1" ) );
    EXPECT_EQ( StringAt( migrated, "CloudType1" ), "" );
}

TEST( SceneCloudSetMigration, AHandEditedValueIsMovedRatherThanJudged )
{
    // §4.4's case: the file a text editor has been in. Whatever the value is, it meant "the kind of cloud
    // this layer is made of" and the KEY is the only thing this step changes — the loader is what decides
    // whether a value names a file, and deciding it twice is how two readers of one field disagree.
    struct Case
    {
        const char*  Name;
        rfl::Generic Value;
    };

    const Case cases[] = {
         { "a raw handle a v4 file might still carry", rfl::Generic( static_cast<int64_t>( 123456789 ) ) },
         { "a path to a file that does not exist",
           rfl::Generic( std::string( "Clouds/Types/Nope.decloudtype" ) ) },
         { "an object", rfl::Generic( rfl::Generic::Object{} ) },
    };

    for ( const Case& c : cases )
    {
        Desert::Assets::EntityData entity;
        entity.Tag = "Sky";

        rfl::Generic::Object payload;
        payload["Enabled"]                   = true;
        payload["CloudType"]                 = c.Value;
        entity.Components["VolumetricCloud"] = rfl::Generic( payload );

        std::vector<Desert::Assets::EntityData> entities{ entity };
        const CloudSetMigrationReport           report = MigrateCloudSetV5ToV6( entities );

        EXPECT_EQ( report.SlotsCarried, 1 ) << c.Name << " was not carried";

        const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );
        EXPECT_FALSE( Has( migrated, "CloudType" ) ) << c.Name << ": the old key survived";
        EXPECT_TRUE( Has( migrated, "CloudType1" ) ) << c.Name << ": the value was dropped rather than moved";

        // The rest of the payload is still there: a strange type is not a reason to lose the layer.
        EXPECT_TRUE( Has( migrated, "Enabled" ) ) << c.Name;
    }
}

TEST( SceneCloudSetMigration, ALayerWithNoTypeKeyIsLeftByteIdentical )
{
    Desert::Assets::EntityData entity;
    entity.Tag = "Sky";

    rfl::Generic::Object payload;
    payload["Enabled"]                   = true;
    payload["Coverage"]                  = 0.25;
    entity.Components["VolumetricCloud"] = rfl::Generic( payload );

    std::vector<Desert::Assets::EntityData> entities{ entity };
    const CloudSetMigrationReport           report = MigrateCloudSetV5ToV6( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.SlotsCarried, 0 );

    const rfl::Generic::Object untouched = CloudPayloadOf( entities.front() );
    EXPECT_FALSE( Has( untouched, "CloudType1" ) ) << "a slot was invented for a layer that named no type";
    EXPECT_TRUE( Has( untouched, "Enabled" ) );
}

TEST( SceneCloudSetMigration, ItIsIdempotent )
{
    std::vector<Desert::Assets::EntityData> entities{
         EntityWithClouds( "Clouds/Types/Stratocumulus.decloudtype" ) };
    MigrateCloudSetV5ToV6( entities );

    const CloudSetMigrationReport second = MigrateCloudSetV5ToV6( entities );
    EXPECT_EQ( second.Entities, 0 );
    EXPECT_EQ( second.SlotsCarried, 0 );

    EXPECT_EQ( StringAt( CloudPayloadOf( entities.front() ), "CloudType1" ),
               "Clouds/Types/Stratocumulus.decloudtype" );
}

TEST( SceneCloudSetMigration, AnEntityWithNoCloudLayerIsLeftAlone )
{
    Desert::Assets::EntityData plain;
    plain.Tag                      = "Cube";
    plain.Components["StaticMesh"] = rfl::Generic( rfl::Generic::Object{} );

    std::vector<Desert::Assets::EntityData> entities{ plain };
    const CloudSetMigrationReport           report = MigrateCloudSetV5ToV6( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_TRUE( entities.front().Components.get( "StaticMesh" ).has_value() );
}

TEST( SceneCloudSetMigration, MigrateSceneRunsItAndStampsTheFileSoItNeverRunsAgain )
{
    SceneSerialized scene;
    scene.SceneName    = "Clouds";
    scene.SceneVersion = kSceneVersionCloudType;
    scene.UnitVersion  = Desert::Migration::kUnitVersion;
    scene.Entities.push_back( EntityWithClouds( "Clouds/Types/Cumulus_Congestus.decloudtype" ) );

    const auto report = MigrateScene( scene );

    EXPECT_TRUE( report.CloudSetRaised );
    EXPECT_EQ( report.CloudSet.SlotsCarried, 1 );
    EXPECT_TRUE( report.Changed() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    // The stamp is the HEAD, which is at or past this step — it was exactly this step until v7 moved the
    // head for the terrain's material. What this line pins is that the file leaves MigrateScene raised past
    // the point this suite is about, not that this suite owns the head.
    EXPECT_GE( kSceneVersion, kSceneVersionCloudSet );

    // Second pass over the stamped tree: nothing left to do.
    const auto again = MigrateScene( scene );
    EXPECT_FALSE( again.Changed() );
}

TEST( SceneCloudSetMigration, TheMigratedFileLoadsIntoTheFirstSlotOfTheComponent )
{
    // THE RELATION THE RENAME IS FOR, and the one a migration can get right on its own terms while still
    // being wrong: the key this function writes has to be the key the REFLECTED serializer reads. A
    // migration that wrote `CloudTypeOne` would pass every assertion above and land every migrated scene
    // on the empty slot, and the symptom would be a sky that quietly reverted to the built-in default —
    // which still looks like weather.
    //
    // Asserted against the reflection table rather than against a string literal, so a field renamed in
    // the component fails here instead of six weeks later.
    // THE KEY THIS FUNCTION WRITES IS THE KEY THE NEXT MIGRATION MOVES — since O1 the reader is no
    // longer the reflected serializer (the slot is a material schema parameter, and the component must
    // NOT carry it) but the v11 -> v12 step, which lifts `CloudType1` into the `.demat`. Asserted by
    // running that step on this one's output: a rename to `CloudTypeOne` would leave the slot stranded
    // in the payload and this test names it.
    const Desert::Reflection::TypeInfo* cloud =
         Desert::Reflection::ReflectionRegistry::Get().Find( "VolumetricCloudData" );
    ASSERT_NE( cloud, nullptr );

    for ( const char* slot : kSlots )
    {
        const auto found =
             std::find_if( cloud->Fields.begin(), cloud->Fields.end(),
                           [slot]( const Desert::Reflection::FieldInfo& field ) { return field.Name == slot; } );
        EXPECT_EQ( found, cloud->Fields.end() )
             << slot << " is a material schema parameter since O1 and must not also be a component field";
    }

    // And the number of slots the schema has is the number of channels the profile table has, which is
    // what fixes the ceiling at four on both sides of the seam.
    EXPECT_EQ( Desert::Graphic::kCloudSpeciesSlots, 4u );

    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( "Clouds/Types/Cirrus.decloudtype" ) };
    MigrateCloudSetV5ToV6( entities );

    EXPECT_TRUE( Has( CloudPayloadOf( entities.front() ), kSlots[0] ) );

    const auto material = Desert::Migration::MigrateCloudMaterialV11ToV12( entities, "Clouds" );
    EXPECT_EQ( material.AssetsMoved, 1 ) << "the slot the set-step wrote is not one the material step moves";
    EXPECT_FALSE( Has( CloudPayloadOf( entities.front() ), kSlots[0] ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
