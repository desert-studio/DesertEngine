// The cloud-type migration, scene v4 -> v5, and the relations it exists to protect.
//
// The kind of cloud a layer is made of stopped being an enumerator compiled into the engine and became an
// ASSET an artist can author, name and ship. One key leaves the file, one changes meaning:
//
//   * "Species" BECOMES "CloudType". The four enumerators ship as four `.decloudtype` files carrying the
//     same twelve numbers T0 compiled in, so the translation is a RENAME rather than a reinterpretation: a
//     scene comes out of this migration rendering the sky it went in with. The value written is a PATH
//     relative to the assets root, because that is what a reflected asset field is written as in this
//     engine (Core::MakeAssetResolver) — a raw handle would be read through the resolver's string branch,
//     fail it, and land the layer on the empty slot with nothing in the log.
//   * "NoiseVolume" is DROPPED, and NAMED while it goes. The slot moved onto the cloud type, because the
//     character of an edge is a property of the kind of cloud; a pure function cannot create the file it
//     would have to move the reference into, so the artist is told which layer lost which volume instead
//     of finding out from a sky that changed. §1.4: a value we drop silently is a value nobody will ever
//     find again.
//
// Everything below runs on the parsed tree. No GPU, no scene graph, no asset manager — the migration is a
// pure function and this is what that buys.

#include <Engine/Assets/CloudTypeData.hpp>
#include <SceneMigration.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using Desert::Migration::CloudTypeMigrationReport;
using Desert::Migration::kSceneVersion;
using Desert::Migration::kSceneVersionCloudSpecies;
using Desert::Migration::kSceneVersionCloudType;
using Desert::Migration::MigrateCloudTypeV4ToV5;
using Desert::Migration::MigrateScene;
using Desert::Migration::SceneSerialized;

namespace
{
    // A v4 cloud payload: the species integer plus a representative selection of the settings that STAY,
    // so the test can assert that nothing else moved.
    rfl::Generic::Object CloudPayloadV4( int species )
    {
        rfl::Generic::Object o;
        o["Enabled"]         = true;
        o["Coverage"]        = 0.25;
        o["Species"]         = static_cast<int64_t>( species );
        o["WeatherTileSize"] = 1200000.0;
        o["DetailStrength"]  = 0.10000000149011612;
        o["ExtinctionScale"] = 8.0;
        return o;
    }

    Desert::Assets::EntityData EntityWithClouds( int species )
    {
        Desert::Assets::EntityData entity;
        entity.Tag                           = "Sky";
        entity.Components["VolumetricCloud"] = rfl::Generic( CloudPayloadV4( species ) );
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

    // The path a migrated payload names, or "" when it names none.
    std::string TypePathOf( const rfl::Generic::Object& object )
    {
        const auto value = object.get( "CloudType" );
        if ( !value.has_value() )
            return {};
        return value.value().to_string().value_or( std::string{} );
    }
} // namespace

TEST( SceneCloudTypeMigration, TheSpeciesBecomesAPathAndTheVolumeSlotGoes )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 2 ) };
    entities.front().Components["VolumetricCloud"] = rfl::Generic(
         []
         {
             rfl::Generic::Object o = CloudPayloadV4( 2 );
             o["NoiseVolume"]       = static_cast<int64_t>( 123456789 );
             return o;
         }() );

    const CloudTypeMigrationReport report = MigrateCloudTypeV4ToV5( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.TypesSet, 1 );
    EXPECT_EQ( report.VolumesLost, 1 );
    EXPECT_EQ( report.FieldsBroken, 0 );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );

    EXPECT_FALSE( Has( migrated, "Species" ) ) << "the species integer survived the migration";
    EXPECT_FALSE( Has( migrated, "NoiseVolume" ) ) << "the layer still carries a noise volume slot";
    EXPECT_EQ( TypePathOf( migrated ), "Clouds/Types/Cumulus_Congestus.decloudtype" );
}

TEST( SceneCloudTypeMigration, EachOfTheFourSpeciesNamesTheFileThatCarriesItsNumbers )
{
    // THE TRANSLATION IS A RENAME. T0's library was ordered flattest-first and its four rows ship as four
    // files; this is the table that says which integer meant which file, and it is written out here rather
    // than derived so that reordering the library breaks a test instead of silently re-pointing every
    // migrated scene at the wrong kind of cloud.
    struct Case
    {
        int         Species;
        const char* Name;
    };

    const Case cases[] = {
         { 0, Desert::Assets::kCloudTypeStratus },
         { 1, Desert::Assets::kCloudTypeCumulusMediocris },
         { 2, Desert::Assets::kCloudTypeCumulusCongestus },
         { 3, Desert::Assets::kCloudTypeCumulonimbus },
    };

    for ( const Case& c : cases )
    {
        std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( c.Species ) };
        MigrateCloudTypeV4ToV5( entities );

        EXPECT_EQ( TypePathOf( CloudPayloadOf( entities.front() ) ),
                   Desert::Assets::CloudTypeAssetRelativePath( c.Name ) )
             << "species " << c.Species << " named the wrong file";
    }
}

TEST( SceneCloudTypeMigration, EverySettingThatStaysIsUntouched )
{
    // The failure mode a "rebuild the object" migration has, and the reason it is worth a test of its own:
    // rebuilding is how you accidentally drop the keys you meant to keep, and the symptom would be a cloud
    // layer that silently returned to its defaults on load.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 1 ) };
    MigrateCloudTypeV4ToV5( entities );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );

    EXPECT_EQ( migrated.get( "Enabled" ).value().to_bool().value_or( false ), true );
    EXPECT_DOUBLE_EQ( migrated.get( "Coverage" ).value().to_double().value_or( 0.0 ), 0.25 );
    EXPECT_DOUBLE_EQ( migrated.get( "WeatherTileSize" ).value().to_double().value_or( 0.0 ), 1200000.0 );
    EXPECT_DOUBLE_EQ( migrated.get( "ExtinctionScale" ).value().to_double().value_or( 0.0 ), 8.0 );
}

TEST( SceneCloudTypeMigration, AMissingSpeciesLeavesTheSlotEmptyRatherThanInventingOne )
{
    // An absent key is how the reflected serializer spells "keep the C++ default", and the C++ default is
    // an empty handle — which is the documented "use the built-in cumulus congestus". Writing a path for a
    // file that never said anything would be a guess about intent, and a scene that has never named a type
    // would start depending on a file existing on disk.
    Desert::Assets::EntityData entity;
    entity.Tag = "Sky";

    rfl::Generic::Object payload;
    payload["Enabled"]                   = true;
    payload["Coverage"]                  = 0.25;
    entity.Components["VolumetricCloud"] = rfl::Generic( payload );

    std::vector<Desert::Assets::EntityData> entities{ entity };
    const CloudTypeMigrationReport          report = MigrateCloudTypeV4ToV5( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.TypesSet, 0 );
    EXPECT_FALSE( Has( CloudPayloadOf( entities.front() ), "CloudType" ) );
}

TEST( SceneCloudTypeMigration, ABrokenSpeciesIsCountedAndTheLayerKeepsTheBuiltInDefault )
{
    // THE HAND-EDITED FILE, which is the case §4.4 of the contract names by name. Three shapes of broken:
    // a species that is not a number at all, one below the library and one above it. None of them says
    // anything about what the author wanted, so none of them is turned into a kind of cloud — and each is
    // COUNTED, because a value that fell through a migration in silence is a value nobody will find.
    struct Case
    {
        const char*  Name;
        rfl::Generic Value;
    };

    const Case cases[] = {
         { "a string", rfl::Generic( std::string( "CumulusCongestus" ) ) },
         { "negative", rfl::Generic( static_cast<int64_t>( -1 ) ) },
         { "past the end of the library", rfl::Generic( static_cast<int64_t>( 4 ) ) },
         { "an object", rfl::Generic( rfl::Generic::Object{} ) },
    };

    for ( const Case& c : cases )
    {
        Desert::Assets::EntityData entity;
        entity.Tag = "Sky";

        rfl::Generic::Object payload;
        payload["Enabled"]                   = true;
        payload["Species"]                   = c.Value;
        entity.Components["VolumetricCloud"] = rfl::Generic( payload );

        std::vector<Desert::Assets::EntityData> entities{ entity };
        const CloudTypeMigrationReport          report = MigrateCloudTypeV4ToV5( entities );

        EXPECT_EQ( report.FieldsBroken, 1 ) << c.Name << " was not counted as broken";
        EXPECT_EQ( report.TypesSet, 0 ) << c.Name << " was turned into a kind of cloud anyway";

        const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );
        EXPECT_FALSE( Has( migrated, "Species" ) ) << c.Name << ": the unusable key survived";
        EXPECT_FALSE( Has( migrated, "CloudType" ) ) << c.Name << ": a kind of cloud was invented for it";

        // The rest of the payload is still there: a broken species is not a reason to lose the layer.
        EXPECT_TRUE( Has( migrated, "Enabled" ) );
    }
}

TEST( SceneCloudTypeMigration, AnEmptyNoiseSlotIsNotReportedAsALoss )
{
    // Every scene in this repository is in exactly this state — the v2 -> v3 migration deliberately left
    // the slot unwritten — so a migration that cried "you lost a volume" over an absent choice would put a
    // warning in front of every artist opening every scene, and warnings nobody can act on are how real
    // ones stop being read.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 0 ) };
    entities.front().Components["VolumetricCloud"] = rfl::Generic(
         []
         {
             rfl::Generic::Object o = CloudPayloadV4( 0 );
             o["NoiseVolume"]       = static_cast<int64_t>( 0 ); // the empty handle, written out
             return o;
         }() );

    const CloudTypeMigrationReport report = MigrateCloudTypeV4ToV5( entities );

    EXPECT_EQ( report.VolumesLost, 0 );
    EXPECT_EQ( report.Entities, 1 ); // the key still leaves the file
    EXPECT_FALSE( Has( CloudPayloadOf( entities.front() ), "NoiseVolume" ) );
}

TEST( SceneCloudTypeMigration, ItIsIdempotent )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 3 ) };
    MigrateCloudTypeV4ToV5( entities );

    const CloudTypeMigrationReport second = MigrateCloudTypeV4ToV5( entities );
    EXPECT_EQ( second.Entities, 0 );
    EXPECT_EQ( second.TypesSet, 0 );
    EXPECT_EQ( second.VolumesLost, 0 );

    // And the path it wrote the first time is still the one it wrote.
    EXPECT_EQ( TypePathOf( CloudPayloadOf( entities.front() ) ),
               Desert::Assets::CloudTypeAssetRelativePath( Desert::Assets::kCloudTypeCumulonimbus ) );
}

TEST( SceneCloudTypeMigration, AnEntityWithNoCloudLayerIsLeftAlone )
{
    Desert::Assets::EntityData plain;
    plain.Tag                      = "Cube";
    plain.Components["StaticMesh"] = rfl::Generic( rfl::Generic::Object{} );

    std::vector<Desert::Assets::EntityData> entities{ plain };
    const CloudTypeMigrationReport          report = MigrateCloudTypeV4ToV5( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_TRUE( entities.front().Components.get( "StaticMesh" ).has_value() );
}

TEST( SceneCloudTypeMigration, MigrateSceneRunsItAndStampsTheFileSoItNeverRunsAgain )
{
    SceneSerialized scene;
    scene.SceneName    = "Clouds";
    scene.SceneVersion = kSceneVersionCloudSpecies;
    scene.UnitVersion  = Desert::Migration::kUnitVersion;
    scene.Entities.push_back( EntityWithClouds( 2 ) );

    const auto report = MigrateScene( scene );

    EXPECT_TRUE( report.CloudTypeRaised );
    EXPECT_EQ( report.CloudType.TypesSet, 1 );
    EXPECT_TRUE( report.Changed() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );

    // AND THE STEP AFTER THIS ONE RAN TOO, on the very key this one wrote. v4 -> v5 turns the species into
    // a `CloudType` path and v5 -> v6 renames that key into the first slot of the set, so a v4 file
    // arrives at the current generation in one call — which is what gating each step on its own version
    // integer is for, and the reason `kSceneVersion` is no longer this step's own number.
    EXPECT_TRUE( report.CloudSetRaised );
    EXPECT_EQ( report.CloudSet.SlotsCarried, 1 );
    EXPECT_GT( kSceneVersion, kSceneVersionCloudType );

    // Second pass over the stamped tree: nothing left to do.
    const auto again = MigrateScene( scene );
    EXPECT_FALSE( again.Changed() );
}

TEST( SceneCloudTypeMigration, ThePathTheMigrationWritesIsThePathTheAssetLayerLooksUnder )
{
    // TWO SIDES OBLIGED TO AGREE, and they are in different modules: this function composes a path out of
    // a constant, and the preloader scans a directory built from the project root. If they part company
    // every migrated scene names a file the scan never loads — and the symptom is not an error but a sky
    // that quietly reverts to the built-in default, which is the hardest kind of defect to notice because
    // it still looks like weather.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 0 ) };
    MigrateCloudTypeV4ToV5( entities );

    const std::string written = TypePathOf( CloudPayloadOf( entities.front() ) );

    EXPECT_EQ( written.rfind( Desert::Assets::kCloudTypeAssetsRelativeDir, 0 ), 0u )
         << "'" << written << "' is not under the directory the preloader scans";
    EXPECT_NE( written.find( Desert::Assets::kCloudTypeExtension ), std::string::npos )
         << "'" << written << "' does not name a cloud type file";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
