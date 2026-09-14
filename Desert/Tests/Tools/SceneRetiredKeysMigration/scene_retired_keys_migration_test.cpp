// THE RETIREMENT PASS: the keys this project RETIRED leave the files, and the Settings block becomes
// exactly what the engine's saver would write.
//
// WHY THE STEP EXISTS AT ALL, since nothing reads a retired key. Until K11 the answer was "the next save
// deletes it anyway" — the saver enumerated its own registry, so any key it did not declare evaporated on
// contact. That was the defect; removing it made this step necessary. A preserved key is preserved whether
// or not we still want it, so wanting rid of one is now a DECISION somebody writes down, and
// Migration::kRetiredKeys is the only place in the repository where that is written.
//
// "Retired" is therefore not "unknown", and the difference has to be in the code rather than in somebody's
// head: an unknown key is another build's and is kept (and named in the load's log); a retired key is ours,
// is dead, and is removed here, once, from the files.
//
// What is asserted:
//
//   1. Every row of kRetiredKeys is removed from the block it names, with its value and reason reported,
//      and nothing else in the block is touched or reordered.
//   2. The pass is idempotent and gated on THE HEAD through MigrateScene — not on a step number of its
//      own, which is the one place this tool departs from "each migration has its own constant". K3 is why:
//      it added five rows to a table the corpus was already past, and a step-number gate would have left
//      every one of them in every file while the tool reported the corpus up to date.
//   3. Canonicalisation produces the SAVER's bytes — the same fields, in the same order, through the same
//      reflection table — and keeps an AssetHandle's on-disk form verbatim.
//   4. THE HEAD ASSERTION, which lives in exactly one suite at a time and has moved here.
//   5. THE RELATION: no retired key is still a reflected setting. A row that named a live field would
//      delete authored data on every run, and the live table is the only thing that can say.

#include <SceneMigration.hpp>
#include <SettingsCanonical.hpp>

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Desert;

namespace
{
    rfl::Generic Settings( const std::string& json )
    {
        const auto parsed = rfl::json::read<rfl::Generic>( json );
        EXPECT_TRUE( parsed.has_value() ) << json;
        return parsed.has_value() ? parsed.value() : rfl::Generic();
    }

    std::vector<std::string> KeysOf( const std::optional<rfl::Generic>& settings )
    {
        std::vector<std::string> keys;
        if ( !settings.has_value() )
            return keys;
        if ( const auto object = settings.value().to_object(); object.has_value() )
            for ( const auto& [key, value] : object.value() )
                keys.push_back( key );
        return keys;
    }

    bool Has( const std::optional<rfl::Generic>& settings, const std::string& key )
    {
        const std::vector<std::string> keys = KeysOf( settings );
        return std::find( keys.begin(), keys.end(), key ) != keys.end();
    }

    std::string ValueOf( const std::optional<rfl::Generic>& settings, const std::string& key )
    {
        if ( !settings.has_value() )
            return {};
        if ( const auto object = settings.value().to_object(); object.has_value() )
            if ( const auto found = object.value().get( key ); found.has_value() )
                return rfl::json::write( found.value() );
        return {};
    }
} // namespace

// ── the retirement ───────────────────────────────────────────────────────────────────────────────

TEST( SceneRetiredKeysMigration, EveryRetiredKeyIsRemovedFromTheBlockItNamesAndReportedWithItsValue )
{
    std::optional<rfl::Generic>     settings = Settings( R"({"Exposure":1.0,"EnableSSGI":true,"Gamma":2.2})" );
    std::vector<Assets::EntityData> entities;

    const auto report = Migration::MigrateRetiredKeys( settings, entities );

    EXPECT_EQ( report.KeysRemoved, 1 );
    ASSERT_EQ( report.RemovedNames.size(), 1u );
    // The name, the VALUE it held, and the reason - not a count. Somebody authored that `true`, and the
    // sentence in the log is what tells them why it is gone (§1.4).
    EXPECT_NE( report.RemovedNames[0].find( "Settings.EnableSSGI=" ), std::string::npos )
         << report.RemovedNames[0];
    EXPECT_NE( report.RemovedNames[0].find( "GlobalIllumination" ), std::string::npos )
         << "the report does not say WHY the key is gone: " << report.RemovedNames[0];

    EXPECT_FALSE( Has( settings, "EnableSSGI" ) );
    // Order and content of everything else, because this file is compared byte for byte now.
    EXPECT_EQ( KeysOf( settings ), ( std::vector<std::string>{ "Exposure", "Gamma" } ) );
}

TEST( SceneRetiredKeysMigration, ABlockStatingNoRetiredKeyIsLeftByteIdentical )
{
    std::optional<rfl::Generic>     settings = Settings( R"({"Exposure":1.0,"Gamma":2.2})" );
    std::vector<Assets::EntityData> entities;
    const std::string               before = rfl::json::write( settings.value() );

    const auto report = Migration::MigrateRetiredKeys( settings, entities );

    EXPECT_EQ( report.KeysRemoved, 0 );
    EXPECT_EQ( rfl::json::write( settings.value() ), before );
}

TEST( SceneRetiredKeysMigration, AMissingSettingsBlockIsNotAFailure )
{
    std::optional<rfl::Generic>     settings; // a scene that states no settings at all
    std::vector<Assets::EntityData> entities;
    const auto                      report = Migration::MigrateRetiredKeys( settings, entities );
    EXPECT_EQ( report.KeysRemoved, 0 );
    EXPECT_FALSE( settings.has_value() );
}

TEST( SceneRetiredKeysMigration, ThePassIsGatedOnTheHeadAndIsIdempotent )
{
    Core::SceneSerialized scene;
    scene.SceneVersion = Migration::kSceneVersionDebugView; // v13: the pass must run
    scene.UnitVersion  = Core::kUnitVersion;
    scene.Settings     = Settings( R"({"EnableSSGI":true})" );

    const auto first = Migration::MigrateScene( scene );
    EXPECT_TRUE( first.RetiredKeysRaised );
    EXPECT_EQ( first.RetiredKeys.KeysRemoved, 1 );
    EXPECT_FALSE( Has( scene.Settings, "EnableSSGI" ) );
    EXPECT_EQ( scene.SceneVersion.value_or( 0 ), Core::kSceneVersion );

    // Hand-edited back in; the gate must not care, because the file now claims the head.
    scene.Settings    = Settings( R"({"EnableSSGI":true})" );
    const auto second = Migration::MigrateScene( scene );
    EXPECT_FALSE( second.RetiredKeysRaised );
    EXPECT_FALSE( second.Changed() );
    EXPECT_TRUE( Has( scene.Settings, "EnableSSGI" ) );
}

// THE ASSERTION K3 NEEDED AND THE OLD GATE WOULD HAVE FAILED. A corpus already stamped at the previous
// head must still be swept by rows added after it — otherwise a retirement written down here is a
// retirement that never happens, and the tool says the files are up to date while every one of them still
// carries the key. This is the whole reason the pass is gated on Core::kSceneVersion rather than on a step
// number: set the gate back to `< kSceneVersionRetiredKeys` and this test goes red.
TEST( SceneRetiredKeysMigration, ARowAddedAfterTheLastHeadStillFiresOnAFileStampedAtThatHead )
{
    Core::SceneSerialized scene;
    scene.SceneVersion = Migration::kSceneVersionRetiredKeys; // v14 — where the whole corpus stood
    scene.UnitVersion  = Core::kUnitVersion;
    scene.Settings     = Settings( R"({"Exposure":1.0,"CloudQualityTier":"Low","AA":"SMAA"})" );

    const auto report = Migration::MigrateScene( scene );

    EXPECT_TRUE( report.RetiredKeysRaised );
    EXPECT_EQ( report.RetiredKeys.KeysRemoved, 2 );
    EXPECT_FALSE( Has( scene.Settings, "CloudQualityTier" ) );
    EXPECT_FALSE( Has( scene.Settings, "AA" ) );
    EXPECT_TRUE( Has( scene.Settings, "Exposure" ) );
}

// K3's five, as a set rather than one by one: the point of the move is that ALL of what a machine can
// afford left the level file, and a table missing one of them is a scene format that still states it.
TEST( SceneRetiredKeysMigration, TheFiveMachineQualityKeysAreRetiredAndReportedWithTheirValues )
{
    std::optional<rfl::Generic> settings =
         Settings( R"({"AA":"SMAA","MeshLOD":false,"TextureFilterMode":"Nearest","Anisotropy":16,)"
                   R"("CloudQualityTier":"Low","Exposure":1.0})" );
    std::vector<Assets::EntityData> entities;

    const auto report = Migration::MigrateRetiredKeys( settings, entities );

    EXPECT_EQ( report.KeysRemoved, 5 );
    EXPECT_EQ( KeysOf( settings ), ( std::vector<std::string>{ "Exposure" } ) );

    // Each one NAMED with the value it held. Nothing is carried into the machine store — 51 scenes and
    // one machine.json cannot be reconciled, so the log is how the person who authored `Anisotropy: 16`
    // finds out to set it once, for the machine, instead of noticing a blurrier picture in six months.
    for ( const char* key : { "AA", "MeshLOD", "TextureFilterMode", "Anisotropy", "CloudQualityTier" } )
    {
        const bool named =
             std::any_of( report.RemovedNames.begin(), report.RemovedNames.end(), [key]( const std::string& line )
                          { return line.find( std::string( "Settings." ) + key + "=" ) != std::string::npos; } );
        EXPECT_TRUE( named ) << key << " was removed without being named with its value";
    }
    const bool saysWhere =
         std::any_of( report.RemovedNames.begin(), report.RemovedNames.end(), []( const std::string& line )
                      { return line.find( "MachineSettings" ) != std::string::npos; } );
    EXPECT_TRUE( saysWhere ) << "the report does not say where these values went";
}

// Г26's three, as a set, for the same reason: what leaves the level file is THE WIND, and a table
// missing one of the three is a scene format that still states part of it. Unlike K3's five these go
// nowhere at all -- there is no store to carry them to, because the meaning of each was defined by the
// one reader (the procedural grass generator, cut at scene schema v18) and it is gone.
TEST( SceneRetiredKeysMigration, TheThreeWindKeysAreRetiredAndReportedWithTheirValues )
{
    std::optional<rfl::Generic> settings =
         Settings( R"({"WindDirection":20.0,"WindStrength":0.15,"WindTurbulence":1.0,"Gravity":981.0})" );
    std::vector<Assets::EntityData> entities;

    const auto report = Migration::MigrateRetiredKeys( settings, entities );

    EXPECT_EQ( report.KeysRemoved, 3 );
    EXPECT_EQ( KeysOf( settings ), ( std::vector<std::string>{ "Gravity" } ) );

    for ( const char* key : { "WindDirection", "WindStrength", "WindTurbulence" } )
    {
        const bool named =
             std::any_of( report.RemovedNames.begin(), report.RemovedNames.end(), [key]( const std::string& line )
                          { return line.find( std::string( "Settings." ) + key + "=" ) != std::string::npos; } );
        EXPECT_TRUE( named ) << key << " was removed without being named with its value";
    }
}

// THE CLOUD LAYER KEEPS ITS OWN WIND, and this is the assertion that says the retirement cut the right
// thing. `VolumetricCloudData::WindDirection` is a live, read field with the same NAME as one of the
// three rows above; the rows name the `Settings` block and the migration removes keys from the block
// they name, so a layer's wind must survive a pass that deletes the scene's.
TEST( SceneRetiredKeysMigration, ACloudLayersOwnWindIsNotTouchedByTheScenesWindRetirement )
{
    std::optional<rfl::Generic> settings = Settings( R"({"WindDirection":20.0})" );

    Assets::EntityData cloud;
    cloud.Components["VolumetricCloud"] =
         rfl::json::read<rfl::Generic>( R"({"WindDirection":[1.0,0.0,0.0],"WindSpeed":7.0})" ).value();
    std::vector<Assets::EntityData> entities = { cloud };

    const auto report = Migration::MigrateRetiredKeys( settings, entities );

    EXPECT_EQ( report.KeysRemoved, 1 );
    EXPECT_FALSE( Has( settings, "WindDirection" ) );

    const auto kept = entities[0].Components.get( "VolumetricCloud" );
    ASSERT_TRUE( kept.has_value() );
    EXPECT_TRUE( Has( kept.value(), "WindDirection" ) )
         << "the cloud layer lost its own wind to a retirement aimed at the scene's";
}

// ── canonicalisation ─────────────────────────────────────────────────────────────────────────────

TEST( SceneRetiredKeysMigration, CanonicalisationStatesEveryFieldTheSaverWouldStateInTheSaversOrder )
{
    const auto* type = Reflection::ReflectionRegistry::Get().Find( "SceneSettings" );
    ASSERT_NE( type, nullptr );

    std::optional<rfl::Generic> settings = Settings( R"({"Exposure":0.26})" );
    const auto                  report   = Migration::CanonicaliseSettings( settings );

    ASSERT_FALSE( report.Refused );
    std::vector<std::string> expected;
    for ( const auto& field : type->Fields )
        expected.push_back( field.Name );
    EXPECT_EQ( KeysOf( settings ), expected )
         << "the canonical block is not the reflection table's field list, in its order";

    // `0.26` is not representable as a float, so the saver's own narrow-and-widen restates it. The VALUE
    // is unchanged - it is the same float either way - and only the TEXT moves. That is the whole reason
    // byte-identity needed a conversion in the files rather than a rule in code.
    EXPECT_EQ( report.ValuesRestated, 1 );
    EXPECT_NE( ValueOf( settings, "Exposure" ), "0.26" );
    EXPECT_FLOAT_EQ( std::stof( ValueOf( settings, "Exposure" ) ), 0.26f );
}

TEST( SceneRetiredKeysMigration, CanonicalisationIsIdempotent )
{
    std::optional<rfl::Generic> settings = Settings( R"({"Exposure":0.26})" );
    ASSERT_FALSE( Migration::CanonicaliseSettings( settings ).Refused );
    const std::string once = rfl::json::write( settings.value() );

    const auto again = Migration::CanonicaliseSettings( settings );
    EXPECT_EQ( again.KeysAdded, 0 );
    EXPECT_EQ( again.ValuesRestated, 0 );
    EXPECT_EQ( rfl::json::write( settings.value() ), once );
}

TEST( SceneRetiredKeysMigration, AnAssetHandleKeepsTheFormTheFileStatedIt )
{
    // An AssetHandle is written as a PATH when the saver has a resolver and as a raw integer when it does
    // not, and this tool has no AssetManager. Restating it would silently change the field's format in the
    // fourteen scenes that carry `"SplashSprite": ""`.
    const auto* type = Reflection::ReflectionRegistry::Get().Find( "SceneSettings" );
    ASSERT_NE( type, nullptr );
    const bool anyHandle =
         std::any_of( type->Fields.begin(), type->Fields.end(), []( const Reflection::FieldInfo& f )
                      { return f.Type == Reflection::FieldType::AssetHandle; } );
    ASSERT_TRUE( anyHandle ) << "SceneSettings has no AssetHandle field, so this assertion is vacuous";

    std::optional<rfl::Generic> settings = Settings( R"({"SplashSprite":""})" );
    ASSERT_FALSE( Migration::CanonicaliseSettings( settings ).Refused );
    EXPECT_EQ( ValueOf( settings, "SplashSprite" ), R"("")" ) << "the path form was replaced by a raw handle";
}

TEST( SceneRetiredKeysMigration, ASceneWithNoSettingsBlockGetsTheCanonicalOne )
{
    // The saver always writes a Settings block, so a file without one can never be byte-stable.
    std::optional<rfl::Generic> settings;
    const auto                  report = Migration::CanonicaliseSettings( settings );
    EXPECT_TRUE( report.BlockCreated );
    ASSERT_TRUE( settings.has_value() );
    EXPECT_FALSE( KeysOf( settings ).empty() );
}

// ── this step's own number, and the relation ─────────────────────────────────────────────────────

// THE STEP'S PLACE, which is all this suite can still say about the head. The HEAD assertion moved on to
// Desert/Tests/Tools/SceneScriptRootMigration when I9 raised Core::kSceneVersion to 16 — it travels with
// the newest step, which is the only suite that can hold it without going red the day the next one lands.
// What stays here is this step's own generation and its place above its predecessor: those are facts
// about the machine-quality retirement and they do not move.
TEST( SceneRetiredKeysMigration, ThisStepSitsAboveItsPredecessor )
{
    EXPECT_EQ( 15, Migration::kSceneVersionMachineQuality );
    EXPECT_GT( Migration::kSceneVersionMachineQuality, Migration::kSceneVersionRetiredKeys );
    EXPECT_LE( Migration::kSceneVersionMachineQuality, Core::kSceneVersion )
         << "a step cannot sit above the head the loader requires";
}

// THE RELATION, and it is why this suite is worth more than its assertions: a row of kRetiredKeys names a
// key that must NOT be a live field. If one ever did, every run of the tool would delete authored data
// from every file that states it — and the only thing that can answer "is this still a field" is the live
// reflection table the saver writes from.
TEST( SceneRetiredKeysMigration, NoRetiredKeyIsStillAReflectedSceneSetting )
{
    const auto* type = Reflection::ReflectionRegistry::Get().Find( "SceneSettings" );
    ASSERT_NE( type, nullptr );

    for ( const Migration::RetiredKey& row : Migration::kRetiredKeys )
    {
        if ( std::string( row.Block ) != "Settings" )
            continue;
        const bool live = std::any_of( type->Fields.begin(), type->Fields.end(),
                                       [&row]( const Reflection::FieldInfo& f ) { return f.Name == row.Key; } );
        EXPECT_FALSE( live ) << "'" << row.Key
                             << "' is retired by the migration AND is still a reflected SceneSettings field, "
                                "so every run of the tool deletes a value somebody authored";
    }
}

// Every row has to say WHY, because the sentence is what reaches the person whose value disappeared. An
// empty reason is a removal nobody can account for a month later.
//
// NO NON-EMPTY GUARD, AND ITS ABSENCE IS THE POINT. This test used to open with
// `ASSERT_GT( std::size( kRetiredKeys ), 0u )`, which made the table's own terminal state - zero rows, every
// retirement written back to every file - a RED test. A table designed to empty cannot be guarded against
// emptying: that is the same defect as declaring it `RetiredKey k[]`, one level up. Zero rows here means
// zero rows to check, and this test is then vacuously true, which is the correct answer.
TEST( SceneRetiredKeysMigration, EveryRetiredRowNamesABlockAKeyAndAReason )
{
    for ( const Migration::RetiredKey& row : Migration::kRetiredKeys )
    {
        EXPECT_TRUE( row.Block != nullptr && row.Block[0] != '\0' );
        EXPECT_TRUE( row.Key != nullptr && row.Key[0] != '\0' );
        EXPECT_GT( std::string( row.Why ).size(), 20u ) << "row '" << row.Key << "' has no usable reason";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
