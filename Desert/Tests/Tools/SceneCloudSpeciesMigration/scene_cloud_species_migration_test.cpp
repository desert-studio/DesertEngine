// The cloud-species migration, scene v3 -> v4, and the relations it exists to protect.
//
// The vertical profile stopped being one analytic curve driven by a scalar and became a per-SPECIES
// table, so a scene file has to name a species instead of a number. Four keys leave the file and one
// arrives:
//
//   * "CloudType" BECOMES "Species". The old scalar ran from "flat sheet low in the layer" to "tall heaped
//     cloud" and the library is ordered along exactly that axis, so the translation is a partition of the
//     scalar rather than a guess. The boundaries are placed so that the component's own former default of
//     0.6 lands on the species the new default names — a scene that carried the default comes out looking
//     like what it was.
//   * "CloudTypeVariance" is DROPPED. It mixed noise into the scalar so that neighbouring clouds would not
//     all reach the same ceiling; the table's second axis is the placement pattern's own value, which
//     answers the same question with height that CORRELATES with how much cloud is there. There is nothing
//     on the component for it to become.
//   * "LayerBottomAltitude" and "LayerThickness" are DROPPED. The shell is now the union of the altitude
//     ranges of the species in the layer and is computed by Graphic::PackCloudParams. An authored shell and
//     a species' own altitudes are two numbers obliged to agree, which is the defect class the whole move
//     removes; carrying the authored pair forward would reintroduce it under a new name.
//
// Everything below runs on the parsed tree. No GPU, no scene graph, no asset manager — the migration is a
// pure function and this is what that buys.

#include <SceneMigration.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>

#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using Desert::Migration::CloudSpeciesMigrationReport;
using Desert::Migration::kSceneVersion;
using Desert::Migration::kSceneVersionCloudNoise;
using Desert::Migration::kSceneVersionCloudSpecies;
using Desert::Migration::MigrateCloudSpeciesV3ToV4;
using Desert::Migration::MigrateScene;
using Desert::Migration::SceneSerialized;
using Desert::Reflection::ReflectionRegistry;

namespace
{
    // The three keys with nowhere to go, as a v3 file spells them.
    const std::vector<std::string> kDroppedKeys = { "LayerBottomAltitude", "LayerThickness", "CloudTypeVariance" };

    // A v3 cloud payload with everything the migration touches present, and a representative selection of
    // the settings that STAY, so the test can assert that nothing else moved.
    rfl::Generic::Object CloudPayloadV3( double cloudType )
    {
        rfl::Generic::Object o;
        o["Enabled"]             = true;
        o["LayerBottomAltitude"] = 300000.0;
        o["LayerThickness"]      = 500000.0;
        o["Coverage"]            = 0.25;
        o["CloudType"]           = cloudType;
        o["CloudTypeVariance"]   = 0.5;
        o["WeatherTileSize"]     = 1200000.0;
        o["DetailStrength"]      = 0.10000000149011612;
        o["ExtinctionScale"]     = 8.0;
        return o;
    }

    Desert::Assets::EntityData EntityWithClouds( double cloudType )
    {
        Desert::Assets::EntityData entity;
        entity.Tag                           = "Sky";
        entity.Components["VolumetricCloud"] = rfl::Generic( CloudPayloadV3( cloudType ) );
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

    // The species a migrated payload names, or -1 when it names none.
    int SpeciesOf( const rfl::Generic::Object& object )
    {
        const auto value = object.get( "Species" );
        if ( !value.has_value() )
            return -1;
        return static_cast<int>( value.value().to_int64().value_or( -1 ) );
    }
} // namespace

TEST( SceneCloudSpeciesMigration, TheFourFieldsGoAndTheSpeciesArrives )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 0.55 ) };

    const CloudSpeciesMigrationReport report = MigrateCloudSpeciesV3ToV4( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.FieldsDropped, 4 ); // the three above plus CloudType, which became the species
    EXPECT_EQ( report.SpeciesSet, 1 );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );

    for ( const std::string& key : kDroppedKeys )
        EXPECT_FALSE( Has( migrated, key ) ) << key << " survived the migration";
    EXPECT_FALSE( Has( migrated, "CloudType" ) ) << "the scalar cloud type survived the migration";

    EXPECT_TRUE( Has( migrated, "Species" ) );
}

TEST( SceneCloudSpeciesMigration, TheScalarPicksTheSpeciesAlongTheAxisItAlreadyRanOn )
{
    // The old scalar's own documentation: "0 is a flat sheet lying in the bottom quarter of the layer; 1
    // is a heaped cloud that fills it". The library is ordered along that axis, so every boundary below is
    // a translation and not a preference — and 0.6, the component's former default, has to land on the
    // species the new default names, or every scene that carried the default changes appearance for no
    // reason its author would recognise.
    // THE INTEGERS ARE THE LIBRARY'S ORDER, flattest first, and they are spelt as numbers here because
    // the enumerator they used to name was deleted when the species became an asset (T1). This migration
    // still writes the OLD key on purpose: v4 -> v5 is what turns it into a handle, and a chain of steps
    // each gated on its own version integer is the arrangement SceneMigration.hpp argues for.
    struct Case
    {
        double Scalar;
        int    Expected;
    };

    const Case cases[] = {
         { 0.0, 0 },  { 0.24, 0 }, { 0.25, 1 }, { 0.45, 1 }, { 0.55, 2 },
         { 0.60, 2 }, { 0.75, 2 }, { 0.85, 3 }, { 1.0, 3 },
    };

    for ( const Case& c : cases )
    {
        std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( c.Scalar ) };
        MigrateCloudSpeciesV3ToV4( entities );

        EXPECT_EQ( SpeciesOf( CloudPayloadOf( entities.front() ) ), static_cast<int>( c.Expected ) )
             << "cloud type " << c.Scalar << " chose the wrong species";
    }

    // The old component's own default, stated as its own assertion because it is the one value whose
    // translation decides whether EVERY scene that never touched the slider changes appearance. 0.6 has to
    // land on index 2 — cumulus congestus — which is the kind of cloud the built-in default type is made
    // of; that those two are the same twelve numbers is Desert/Tests/Engine/CloudType's assertion, and
    // this is the half of the relation that lives on this side of the migration.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 0.6 ) };
    MigrateCloudSpeciesV3ToV4( entities );
    EXPECT_EQ( SpeciesOf( CloudPayloadOf( entities.front() ) ), 2 )
         << "a scene carrying the old default no longer carries the new one";
}

TEST( SceneCloudSpeciesMigration, EverySettingThatStaysIsUntouched )
{
    // The failure mode a "rebuild the object" migration has, and the reason it is worth a test of its own:
    // rebuilding is how you accidentally drop the keys you meant to keep, and the symptom would be a cloud
    // layer that silently returned to its defaults on load.
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 0.55 ) };
    MigrateCloudSpeciesV3ToV4( entities );

    const rfl::Generic::Object migrated = CloudPayloadOf( entities.front() );

    EXPECT_TRUE( Has( migrated, "Enabled" ) );
    EXPECT_TRUE( Has( migrated, "Coverage" ) );
    EXPECT_TRUE( Has( migrated, "WeatherTileSize" ) );
    EXPECT_TRUE( Has( migrated, "DetailStrength" ) );
    EXPECT_TRUE( Has( migrated, "ExtinctionScale" ) );

    // Four keys out, one in.
    EXPECT_EQ( migrated.size(), CloudPayloadV3( 0.55 ).size() - 3u );

    const auto coverage = migrated.get( "Coverage" );
    ASSERT_TRUE( coverage.has_value() );
    EXPECT_DOUBLE_EQ( coverage.value().to_double().value_or( -1.0 ), 0.25 );
}

TEST( SceneCloudSpeciesMigration, APayloadWithNoCloudTypeKeepsTheDefaultSpeciesRatherThanInventingOne )
{
    // An absent key is how the reflection serializer spells "keep the C++ default". A file that never said
    // anything about its cloud type is a file whose author expressed no preference, and writing Stratus
    // into it would be a guess about intent dressed up as a migration.
    rfl::Generic::Object payload;
    payload["Enabled"]        = true;
    payload["LayerThickness"] = 500000.0;

    Desert::Assets::EntityData entity;
    entity.Tag                           = "Sky";
    entity.Components["VolumetricCloud"] = rfl::Generic( payload );

    std::vector<Desert::Assets::EntityData> entities{ entity };
    const CloudSpeciesMigrationReport       report = MigrateCloudSpeciesV3ToV4( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.FieldsDropped, 1 );
    EXPECT_EQ( report.SpeciesSet, 0 );
    EXPECT_FALSE( Has( CloudPayloadOf( entities.front() ), "Species" ) );
}

TEST( SceneCloudSpeciesMigration, ACloudTypeThatIsNotANumberIsSurvivedRatherThanGuessedAt )
{
    // One hand-edited scene away, and the answer is the C++ default plus a warning rather than a number
    // pulled out of a string.
    rfl::Generic::Object payload;
    payload["Enabled"]   = true;
    payload["CloudType"] = std::string( "cumulus" );

    Desert::Assets::EntityData entity;
    entity.Tag                           = "Sky";
    entity.Components["VolumetricCloud"] = rfl::Generic( payload );

    std::vector<Desert::Assets::EntityData> entities{ entity };
    const CloudSpeciesMigrationReport       report = MigrateCloudSpeciesV3ToV4( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.SpeciesSet, 0 );
    EXPECT_FALSE( Has( CloudPayloadOf( entities.front() ), "Species" ) );
    EXPECT_FALSE( Has( CloudPayloadOf( entities.front() ), "CloudType" ) );
}

TEST( SceneCloudSpeciesMigration, ItIsIdempotent )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWithClouds( 0.55 ) };
    MigrateCloudSpeciesV3ToV4( entities );

    const CloudSpeciesMigrationReport second = MigrateCloudSpeciesV3ToV4( entities );
    EXPECT_EQ( second.Entities, 0 );
    EXPECT_EQ( second.FieldsDropped, 0 );
    EXPECT_EQ( second.SpeciesSet, 0 );

    // And the species it wrote the first time is still the one it wrote.
    EXPECT_EQ( SpeciesOf( CloudPayloadOf( entities.front() ) ), 2 ); // cumulus congestus
}

TEST( SceneCloudSpeciesMigration, AnEntityWithNoCloudLayerIsLeftAlone )
{
    Desert::Assets::EntityData plain;
    plain.Tag                      = "Cube";
    plain.Components["StaticMesh"] = rfl::Generic( rfl::Generic::Object{} );

    std::vector<Desert::Assets::EntityData> entities{ plain };
    const CloudSpeciesMigrationReport       report = MigrateCloudSpeciesV3ToV4( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_TRUE( entities.front().Components.get( "StaticMesh" ).has_value() );
}

TEST( SceneCloudSpeciesMigration, ACloudPayloadThatIsNotAnObjectIsSurvivedRatherThanCrashedOn )
{
    Desert::Assets::EntityData broken;
    broken.Tag                           = "Sky";
    broken.Components["VolumetricCloud"] = rfl::Generic( 7.0 );

    std::vector<Desert::Assets::EntityData> entities{ broken };
    const CloudSpeciesMigrationReport       report = MigrateCloudSpeciesV3ToV4( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.FieldsDropped, 0 );
}

TEST( SceneCloudSpeciesMigration, MigrateSceneRunsItAndStampsTheFileSoItNeverRunsAgain )
{
    SceneSerialized scene;
    scene.SceneName    = "Clouds";
    scene.SceneVersion = kSceneVersionCloudNoise;
    scene.UnitVersion  = Desert::Migration::kUnitVersion;
    scene.Entities.push_back( EntityWithClouds( 0.55 ) );

    const auto report = MigrateScene( scene );

    EXPECT_TRUE( report.CloudSpeciesRaised );
    EXPECT_EQ( report.CloudSpecies.FieldsDropped, 4 );
    EXPECT_TRUE( report.Changed() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );

    // AND THE STEP AFTER THIS ONE RAN TOO, on the key this one wrote. The chain is the point: a v3 file
    // arrives with a scalar, leaves with a `.decloudtype` path, and the species integer exists only in
    // between. Asserting it here is what keeps the two steps in the right ORDER — reversed, the v4 -> v5
    // step would find no Species and the scene would silently fall back to the built-in default.
    EXPECT_TRUE( report.CloudTypeRaised );
    EXPECT_EQ( report.CloudType.TypesSet, 1 );
    EXPECT_FALSE( Has( CloudPayloadOf( scene.Entities.front() ), "Species" ) );

    // AND THE STEP AFTER THAT ONE RAN TOO. The chain is three long now: a v3 file arrives with a scalar,
    // becomes a species integer, becomes a `.decloudtype` path, and ends as the FIRST SLOT of a set of
    // four. Each of the three intermediate keys exists for exactly one version, which is what gating each
    // step on its own version integer buys and what asserting the whole chain here protects.
    EXPECT_TRUE( report.CloudSetRaised );
    EXPECT_EQ( report.CloudSet.SlotsCarried, 1 );
    EXPECT_FALSE( Has( CloudPayloadOf( scene.Entities.front() ), "CloudType" ) );

    // AND THE CHAIN GREW A FOURTH LINK WITH O1: the slot the set-step wrote is moved into the cloud
    // MATERIAL by v11 -> v12, so at the head it lives in the `.demat` the payload names, not in the
    // payload. The material step's own suite (SceneCloudMaterialMigration) owns the file's contents;
    // what this chain assertion keeps is that the slot LEFT through the material step rather than
    // evaporating.
    EXPECT_TRUE( report.CloudMaterialRaised );
    EXPECT_EQ( report.CloudMaterial.AssetsMoved, 1 );
    EXPECT_FALSE( Has( CloudPayloadOf( scene.Entities.front() ), "CloudType1" ) );
    EXPECT_TRUE( Has( CloudPayloadOf( scene.Entities.front() ), "Material" ) );

    // Second pass over the stamped tree: nothing left to do.
    const auto again = MigrateScene( scene );
    EXPECT_FALSE( again.Changed() );
}

// ---------------------------------------------------------------------------------------------------
// The other half of "the old path is gone": the struct itself.
// ---------------------------------------------------------------------------------------------------

TEST( SceneCloudSpeciesMigration, TheReflectedComponentNoLongerCarriesWhatTheMigrationDeletes )
{
    const auto* type = ReflectionRegistry::Get().Find( "VolumetricCloudData" );
    ASSERT_NE( type, nullptr );

    // "CloudType" is NOT in this list any more, and its absence is the point: the name came back in T1 as
    // the ASSET HANDLE that replaced the species enumerator. What must stay gone is the SCALAR, and the
    // reflected field's type is what says which of the two it is — asserted below.
    for ( const std::string& key : { std::string( "LayerBottomAltitude" ), std::string( "LayerThickness" ),
                                     std::string( "CloudTypeVariance" ) } )
    {
        const bool present = std::any_of( type->Fields.begin(), type->Fields.end(),
                                          [&key]( const auto& field ) { return field.Name == key; } );
        EXPECT_FALSE( present ) << key
                                << " is still a reflected field, so the migration removes a value the "
                                   "struct can still hold — that is silent data loss";
    }
}

TEST( SceneCloudSpeciesMigration, TheReflectedComponentCarriesTheSlotThatReplacedThem )
{
    const auto* type = ReflectionRegistry::Get().Find( "VolumetricCloudData" );
    ASSERT_NE( type, nullptr );

    // THE SPECIES ENUMERATOR IS GONE TOO, one version later. This test used to assert that the component
    // carried a four-valued enum; T1 replaced it with a handle to a `.decloudtype`, because an enumerator
    // is a kind of cloud nobody can author, name, duplicate or ship — which was the whole request. The
    // key this migration writes therefore lives for exactly one version, between v3 -> v4 and v4 -> v5.
    const auto species = std::find_if( type->Fields.begin(), type->Fields.end(),
                                       []( const auto& field ) { return field.Name == "Species"; } );
    EXPECT_EQ( species, type->Fields.end() )
         << "the species is still an enumerator on the component, so the asset did not replace it";

    // AND THE SLOT IS THE FIRST OF FOUR since T3, because a layer carries a SET of kinds of cloud. The
    // singular name is gone rather than kept for the first of them: `CloudType` beside `CloudType2` reads
    // as one field of a different kind next to three of another.
    EXPECT_EQ( std::find_if( type->Fields.begin(), type->Fields.end(),
                             []( const auto& field ) { return field.Name == "CloudType"; } ),
               type->Fields.end() )
         << "the singular cloud type slot is still a field, so the set did not replace it";

    // THE SLOT MOVED AGAIN WITH O1 — off the component entirely, into the cloud MATERIAL's schema
    // (CloudRaymarch.shader, `CloudType CloudType1`). What replaced the four deleted fields is therefore
    // no longer a reflected field at all; Desert/Tests/Engine/CloudMaterialSchema asserts the schema
    // carries exactly four CloudType slots, which is this assertion continued on the other side of the
    // seam. What this suite keeps is that no SECOND copy grows back here.
    EXPECT_EQ( std::find_if( type->Fields.begin(), type->Fields.end(),
                             []( const auto& field ) { return field.Name == "CloudType1"; } ),
               type->Fields.end() )
         << "the cloud type slot is a material schema parameter since O1 and must not have a second life "
            "on the component";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
