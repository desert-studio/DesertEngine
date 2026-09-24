// The world-unit migration, metres -> centimetres, and the version stamp that is supposed to stop it
// running twice.
//
// WHY THIS TEST EXISTS. The migration was correct and had never been written down: no scene in the
// repository carried a UnitVersion, so the loader re-ran it on EVERY load, the files stayed permanently
// authored in metres, and a scene someone authored correctly in world units was silently multiplied by a
// hundred the first time it was opened. A migration is only "once" if something writes the stamp back and
// something else refuses to run when it sees it - so the two properties asserted hardest below are
//
//   * a STAMPED tree comes out of MigrateScene byte-identical, and
//   * an UNSTAMPED tree is scaled exactly once, no matter how many times MigrateScene is called.
//
// Everything is a value comparison on a parsed tree. That is the whole point of the migration being pure:
// no renderer, no scene, no asset manager, and the fixtures are the on-disk JSON of real shipped scenes.

#include <SceneMigration.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Migration::FileMigrationReport;
using Desert::Migration::kSceneVersion;
using Desert::Migration::kUnitVersion;
using Desert::Migration::MigrateMetresToUnits;
using Desert::Migration::MigrateScene;
using Desert::Migration::SceneSerialized;
using Desert::Migration::UnitMigrationReport;

namespace
{
    // Editor/Resources/Assets/Scenes/CornellDemo.desce, trimmed to the entities that carry a length and
    // copied verbatim - the exact doubles included. A metres-era file: no UnitVersion anywhere.
    constexpr const char* kUnstampedScene = R"({
        "SceneName":"CornellDemo",
        "Entities":[
            {"id":1,"Tag":"CB_Floor","Translation":[0,0,0],"Rotation":[0,0,0],"Scale":[6.0,0.2,6.0],
             "StaticMesh":{"Primitive":"Cube"}},
            {"id":2,"Tag":"CB_Statue","Translation":[0.0,1.5,-1.2],"Rotation":[0,0,0],"Scale":[1.4,1.4,1.4],
             "StaticMesh":{"MeshPath":"Resources/Assets/Meshes/statue.fbx"}},
            {"id":3,"Tag":"CB_BackLight","Translation":[0.0,2.5,-2.5],"Rotation":[0,0,0],"Scale":[1,1,1],
             "PointLight":{"Color":[1.0,0.85,0.6],"Intensity":8.0,"Radius":12.0,"MinRadius":0.0}},
            {"id":4,"Tag":"Camera","Translation":[0.0,2.5,7.0],"Rotation":[0,0,0],"Scale":[1,1,1],
             "Camera":{"IsMainCamera":true,"FOV":45.0,"Near":0.1,"Far":500.0}},
            {"id":5,"Tag":"Sign","Translation":[-2.2,3.4,-3.0],"Rotation":[0,0,0],"Scale":[1,1,1],
             "Text":{"Text":"Desert Engine","FontPath":"Resources/Fonts/Roboto-Regular.ttf","Size":0.8}},
            {"id":6,"Tag":"Ground","Translation":[0,0,0],"Rotation":[0,0,0],"Scale":[1,1,1],
             "Terrain":{"Size":400.0,"Resolution":64,"HeightScale":12.0,"GrassHeight":0.4}}
        ],
        "Settings":{"Gravity":9.81},
        "SceneVersion":1
    })";

    // Indices into kUnstampedScene's entity list, named so an assertion cannot quietly read the wrong one.
    constexpr std::size_t kFloor  = 0; // procedural Cube
    constexpr std::size_t kStatue = 1; // file-backed mesh
    constexpr std::size_t kLight  = 2;
    constexpr std::size_t kCamera = 3;
    constexpr std::size_t kSign   = 4;
    constexpr std::size_t kGround = 5;

    SceneSerialized Parse( const char* json )
    {
        auto parsed = rfl::json::read<SceneSerialized>( json );
        EXPECT_TRUE( parsed ) << ( parsed ? "" : parsed.error().what() );
        return parsed.value_or( SceneSerialized{} );
    }

    std::string Json( const SceneSerialized& scene )
    {
        return rfl::json::write( scene );
    }

    // One payload field, as the tree holds it. Returns a sentinel that no fixture uses so a missing key
    // fails loudly instead of comparing equal to zero.
    double Field( const SceneSerialized& scene, std::size_t entity, const char* component, const char* field )
    {
        const auto& e       = scene.Entities.at( entity );
        const auto  payload = e.Components.get( component );
        EXPECT_TRUE( payload.has_value() ) << component << " payload missing";
        if ( !payload.has_value() )
            return -1.0;
        const auto obj = payload.value().to_object();
        EXPECT_TRUE( obj.has_value() ) << component << " payload is not an object";
        if ( !obj.has_value() )
            return -1.0;
        const auto value = obj.value().get( field );
        EXPECT_TRUE( value.has_value() ) << component << "." << field << " missing";
        if ( !value.has_value() )
            return -1.0;
        if ( const auto d = value.value().to_double(); d.has_value() )
            return d.value();
        if ( const auto i = value.value().to_int64(); i.has_value() )
            return static_cast<double>( i.value() );
        ADD_FAILURE() << component << "." << field << " is not a number";
        return -1.0;
    }

    // Whether a payload STATES a key at all. Separate from Field() on purpose: Field() fails loudly on a
    // missing key, which is right when a value is expected and useless when ABSENCE is the assertion.
    bool Has( const SceneSerialized& scene, std::size_t entity, const char* component, const char* field )
    {
        const auto payload = scene.Entities.at( entity ).Components.get( component );
        if ( !payload.has_value() )
            return false;
        const auto obj = payload.value().to_object();
        if ( !obj.has_value() )
            return false;
        return obj.value().get( field ).has_value();
    }

    double Gravity( const SceneSerialized& scene )
    {
        EXPECT_TRUE( scene.Settings.has_value() );
        if ( !scene.Settings.has_value() )
            return -1.0;
        const auto obj = scene.Settings->to_object();
        EXPECT_TRUE( obj.has_value() );
        if ( !obj.has_value() )
            return -1.0;
        const auto g = obj.value().get( "Gravity" );
        EXPECT_TRUE( g.has_value() );
        return g.has_value() ? g.value().to_double().value_or( -1.0 ) : -1.0;
    }

    EntityData Entity( const char* tag )
    {
        EntityData e;
        e.Tag = std::string( tag );
        return e;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// T1 - a STAMPED scene loads unchanged. The defect this whole task is about, stated as an assertion.
// ---------------------------------------------------------------------------------------------------
TEST( SceneUnitMigration, StampedSceneIsLeftByteIdentical )
{
    SceneSerialized scene = Parse( kUnstampedScene );
    scene.UnitVersion     = kUnitVersion;
    // "Current" means current on BOTH counters. This line used to be absent, because when the test was
    // written the only other migration was gated on a version an unstamped scene already satisfied. It is
    // here now so that adding a third migration cannot make this test quietly assert nothing: a fixture
    // that is not fully stamped would go through MigrateScene and the "byte-identical" claim would be
    // about a tree that never had anything to migrate.
    scene.SceneVersion = kSceneVersion;

    const std::string          before = Json( scene );
    const FileMigrationReport  report = MigrateScene( scene );

    EXPECT_FALSE( report.UnitsRaised ) << "a stamped scene must not be migrated again";
    EXPECT_FALSE( report.SkyRaised );
    EXPECT_FALSE( report.Changed() );
    EXPECT_EQ( Json( scene ), before ) << "MigrateScene rewrote a scene that was already current";
}

// ---------------------------------------------------------------------------------------------------
// T2 - an UNSTAMPED scene is migrated EXACTLY once: every length x100, and a second call is a no-op.
// ---------------------------------------------------------------------------------------------------
TEST( SceneUnitMigration, UnstampedSceneIsMigratedExactlyOnce )
{
    SceneSerialized scene = Parse( kUnstampedScene );

    const FileMigrationReport first = MigrateScene( scene );
    EXPECT_TRUE( first.UnitsRaised );
    EXPECT_EQ( first.Units.Rejected, 0 );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kUnitSchemaTag ), kUnitVersion )
         << "the migration did not stamp the file";

    // Every number the census names, at its authored value x100.
    EXPECT_DOUBLE_EQ( Field( scene, kLight, "PointLight", "Radius" ), 1200.0 );
    EXPECT_DOUBLE_EQ( Field( scene, kLight, "PointLight", "MinRadius" ), 0.0 );
    EXPECT_DOUBLE_EQ( Field( scene, kCamera, "Camera", "Near" ), 10.0 );
    EXPECT_DOUBLE_EQ( Field( scene, kCamera, "Camera", "Far" ), 50000.0 );
    EXPECT_NEAR( Field( scene, kSign, "Text", "Size" ), 80.0, 1e-9 );
    EXPECT_DOUBLE_EQ( Field( scene, kGround, "Terrain", "Size" ), 40000.0 );
    EXPECT_DOUBLE_EQ( Field( scene, kGround, "Terrain", "HeightScale" ), 1200.0 );
    // `Terrain.GrassHeight` is authored in the fixture above and is NOT asserted at 40.0 here, because
    // the chain does not leave it behind to assert: it named the procedural grass generator's blade
    // height, the units census stopped claiming it with Г25, and the v17 -> v18 step removes it from the
    // payload outright. Asserted absent rather than dropped from the test — a length that stops being
    // converted and a length that is silently skipped look identical until someone says which happened.
    EXPECT_FALSE( Has( scene, kGround, "Terrain", "GrassHeight" ) )
         << "the v17 -> v18 step must take the grass generator's blade height out of the payload";
    EXPECT_NEAR( Gravity( scene ), 981.0, 1e-9 );

    // Fields that are NOT lengths keep their authored value - a census that over-reaches inflates a scene
    // by a hundred just as surely as one that under-reaches leaves it in metres.
    EXPECT_DOUBLE_EQ( Field( scene, kCamera, "Camera", "FOV" ), 45.0 );
    EXPECT_DOUBLE_EQ( Field( scene, kLight, "PointLight", "Intensity" ), 8.0 );
    EXPECT_DOUBLE_EQ( Field( scene, kGround, "Terrain", "Resolution" ), 64.0 );

    // ...and now the whole point: running it again changes nothing at all.
    const std::string          afterFirst = Json( scene );
    const FileMigrationReport  second     = MigrateScene( scene );

    EXPECT_FALSE( second.UnitsRaised );
    EXPECT_FALSE( second.Changed() );
    EXPECT_EQ( Json( scene ), afterFirst ) << "the second migration moved the scene - the stamp is not held";

    // A third and fourth, because "once" is a property of the function and not of the number two.
    MigrateScene( scene );
    MigrateScene( scene );
    EXPECT_EQ( Json( scene ), afterFirst );
}

// ---------------------------------------------------------------------------------------------------
// T3 - transforms: Translation always, Scale only for a mesh the engine does NOT regenerate.
// ---------------------------------------------------------------------------------------------------
TEST( SceneUnitMigration, ProceduralPrimitiveKeepsItsScale )
{
    SceneSerialized scene = Parse( kUnstampedScene );
    MigrateScene( scene );

    // CB_Floor is a procedural Cube: the factory rebuilds its geometry at the new size, so scaling the
    // Scale too would cube the object.
    const auto& floorScale = scene.Entities[kFloor].Scale;
    ASSERT_TRUE( floorScale.has_value() );
    EXPECT_FLOAT_EQ( floorScale->x, 6.0f );
    EXPECT_FLOAT_EQ( floorScale->y, 0.2f );

    // CB_Statue is a FILE-backed mesh: nothing regenerates it, so its Scale is a real length.
    const auto& statueScale = scene.Entities[kStatue].Scale;
    ASSERT_TRUE( statueScale.has_value() );
    EXPECT_FLOAT_EQ( statueScale->x, 140.0f );

    // Position is a length either way.
    const auto& floorPos  = scene.Entities[kFloor].Translation;
    const auto& statuePos = scene.Entities[kStatue].Translation;
    ASSERT_TRUE( floorPos.has_value() && statuePos.has_value() );
    EXPECT_FLOAT_EQ( floorPos->y, 0.0f );
    EXPECT_FLOAT_EQ( statuePos->y, 150.0f );
    EXPECT_FLOAT_EQ( statuePos->z, -120.0f );
}

// ---------------------------------------------------------------------------------------------------
// T4 - an ABSENT key is a default, not a metres-era length. This is the one place the tree migration is
// deliberately unlike the live-scene version it replaced, so it is pinned rather than left to drift:
// MainMenu.desce's 57 UI entities carry no Scale at all, and scaling the component default they are
// created with would have turned every one of them into a hundred-times-oversized panel.
// ---------------------------------------------------------------------------------------------------
TEST( SceneUnitMigration, AbsentKeysAreNotInvented )
{
    SceneSerialized scene;
    scene.Entities.push_back( Entity( "UI Panel" ) ); // no Translation, no Rotation, no Scale
    scene.Entities.push_back( Entity( "Empty" ) );
    // Stamped at the current SCENE generation so the only thing running here is the unit migration,
    // which is what this test is about. The tonemapper migration (v1 -> v2) deliberately DOES create a
    // settings block on a scene that has none - see SceneTonemapMigration - and letting it run here
    // would turn this assertion into a test of the wrong migration.
    scene.SceneVersion = kSceneVersion;

    const FileMigrationReport report = MigrateScene( scene );

    EXPECT_TRUE( report.UnitsRaised );
    EXPECT_EQ( report.Units.Entities, 0 );
    EXPECT_EQ( report.Units.Values, 0 );
    EXPECT_FALSE( scene.Entities[0].Translation.has_value() );
    EXPECT_FALSE( scene.Entities[0].Scale.has_value() );

    // A scene with no Settings block keeps not having one; a Settings block with no Gravity keeps its
    // default rather than gaining a key the author never wrote.
    EXPECT_FALSE( scene.Settings.has_value() );

    SceneSerialized withSettings;
    withSettings.SceneVersion = kSceneVersion; // same reason as above
    rfl::Generic::Object empty;
    empty["ShowGrid"]     = true;
    withSettings.Settings = rfl::Generic( empty );
    MigrateScene( withSettings );
    ASSERT_TRUE( withSettings.Settings.has_value() );
    const auto obj = withSettings.Settings->to_object();
    ASSERT_TRUE( obj.has_value() );
    EXPECT_FALSE( obj.value().get( "Gravity" ).has_value() );
}

// ---------------------------------------------------------------------------------------------------
// T5 - a value that is present but unusable is REPORTED and LEFT ALONE, never guessed at. DC 1.4: a
// number we silently replaced is a number nobody will ever find again.
// ---------------------------------------------------------------------------------------------------
TEST( SceneUnitMigration, MalformedValuesAreRejectedNotGuessed )
{
    SceneSerialized scene;

    EntityData           bad = Entity( "Broken" );
    rfl::Generic::Object terrain;
    terrain["Size"]           = std::string( "four hundred" ); // a string where a number belongs
    terrain["HeightScale"]    = 12.0;                          // and one that is fine, beside it
    bad.Components["Terrain"] = rfl::Generic( terrain );

    rfl::Generic::Object collider;
    collider["HalfExtents"]    = rfl::Generic::Array{ 1.0, 2.0 }; // two elements, not three
    bad.Components["Collider"] = rfl::Generic( collider );

    scene.Entities.push_back( std::move( bad ) );

    const FileMigrationReport report = MigrateScene( scene );

    EXPECT_EQ( report.Units.Rejected, 2 );
    EXPECT_EQ( report.Units.Values, 1 ); // HeightScale still went through
    EXPECT_DOUBLE_EQ( Field( scene, 0, "Terrain", "HeightScale" ), 1200.0 );

    // The bad values are still exactly as authored...
    const auto terrainOut = scene.Entities[0].Components.get( "Terrain" ).value().to_object().value();
    EXPECT_EQ( terrainOut.get( "Size" ).value().to_string().value_or( "" ), "four hundred" );
    const auto colliderOut = scene.Entities[0].Components.get( "Collider" ).value().to_object().value();
    EXPECT_EQ( colliderOut.get( "HalfExtents" ).value().to_array().value().size(), 2u );

    // ...and the file is stamped anyway, because a rejected value is not a reason to migrate the whole
    // scene a second time next load and reject it again.
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kUnitSchemaTag ), kUnitVersion );
}

// ---------------------------------------------------------------------------------------------------
// T6 - the relation the stamp exists to enforce. MigrateMetresToUnits itself is ungated and WILL scale
// twice if called twice; MigrateScene is what makes it once. Asserting both halves is what stops someone
// "simplifying" the gate away and re-creating the original defect.
// ---------------------------------------------------------------------------------------------------
TEST( SceneUnitMigration, TheStampIsWhatStopsTheSecondPass )
{
    SceneSerialized gated = Parse( kUnstampedScene );
    MigrateScene( gated );
    MigrateScene( gated );
    EXPECT_DOUBLE_EQ( Field( gated, kGround, "Terrain", "Size" ), 40000.0 );

    SceneSerialized ungated = Parse( kUnstampedScene );
    MigrateMetresToUnits( ungated.Entities, ungated.Settings );
    MigrateMetresToUnits( ungated.Entities, ungated.Settings );
    EXPECT_DOUBLE_EQ( Field( ungated, kGround, "Terrain", "Size" ), 4000000.0 )
         << "the raw migration is expected to compound - the gate is the only thing that makes it once";
}

// ---------------------------------------------------------------------------------------------------
// T7 - both version integers are independent, and a scene missing BOTH gets both raised in one pass.
// ---------------------------------------------------------------------------------------------------
TEST( SceneUnitMigration, BothVersionsAreRaisedIndependently )
{
    SceneSerialized old = Parse( kUnstampedScene );
    old.SceneVersion    = std::nullopt; // sky schema v0 as well as units v0

    EntityData           sky = Entity( "Sky" );
    rfl::Generic::Object skybox;
    skybox["Procedural"]     = true;
    skybox["SunIntensity"]   = 22.0;
    skybox["SunDiskRadius"]  = 0.02;
    sky.Components["Skybox"] = rfl::Generic( skybox );
    old.Entities.push_back( std::move( sky ) );

    const FileMigrationReport report = MigrateScene( old );

    EXPECT_TRUE( report.SkyRaised );
    EXPECT_TRUE( report.UnitsRaised );
    EXPECT_EQ( report.Sky.Entities, 1 );
    EXPECT_EQ( Desert::Assets::StatedVersion( old.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    EXPECT_EQ( Desert::Assets::StatedVersion( old.Header, Desert::Assets::kUnitSchemaTag ), kUnitVersion );

    // The sky payload the sky migration wrote carries no length, so the unit migration cannot have
    // touched it: 22 is an intensity and 2.29 is an angle in degrees.
    const auto skyOut = old.Entities.back().Components.get( "SkyAtmosphere" ).value().to_object().value();
    EXPECT_DOUBLE_EQ( skyOut.get( "SunIntensity" ).value().to_double().value_or( -1.0 ), 22.0 );
    EXPECT_NEAR( skyOut.get( "SunAngularDiameter" ).value().to_double().value_or( -1.0 ), 2.29183, 1e-4 );

    // ...and the lengths beside it still moved.
    EXPECT_DOUBLE_EQ( Field( old, kCamera, "Camera", "Far" ), 50000.0 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
