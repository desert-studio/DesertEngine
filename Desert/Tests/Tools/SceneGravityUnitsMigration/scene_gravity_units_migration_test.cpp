// The v8 -> v9 gravity-units migration, and the relation it exists to make permanent.
//
// `SceneSettings::Gravity` was authored in Details, saved into 45 scenes, and read by NOBODY: the physics
// world set its gravity from a literal `-981.0f` in PhysicsWorld.cpp. Two statements of one value, and the
// live one was the one with no UI. That is DEV_CONTRACT §1.3 (a dead setting) and §2 (one source of truth).
//
// It was harmless only because of that. 39 of those scenes carried the metre-era 9.81 under a centimetre
// stamp — a hundred times too weak — and 4 more carried 981.0000419616699, which is exactly
// 9.8100004196167 x 100: the arithmetic fingerprint of an earlier pass that reached some files and not
// others. The literal is deleted in the same change that adds this migration, so from v9 on the value in
// the file is the value the world falls at, and a wrong one is visible instead of inert.
//
// Five things are asserted:
//
//   1. The pure function restates the two Earth spellings and nothing else.
//   2. It REFUSES to guess at any other value, because a threshold cannot tell a deliberately weak gravity
//      from a forgotten metre-era one.
//   3. It survives the shapes a hand-edited file has: an integer, a missing key, a wrong type, no Settings
//      block at all — and leaves the rest of the block untouched and in order.
//   4. It is idempotent and gated on its OWN version integer.
//   5. THE RELATION: what the migration writes and what SceneSettings defaults to are one number, and no
//      shipped scene carries a metre-era value. That last one is the class this defect belongs to.

#include <SceneMigration.hpp>

#include <Engine/Core/SceneSettings.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Desert;

namespace
{
    // A Settings block with a Gravity of the given spelling, plus two neighbours so the test can prove the
    // rest of the block survives untouched and in order.
    std::optional<rfl::Generic> SettingsWith( rfl::Generic gravity )
    {
        rfl::Generic::Object o;
        o["Exposure"] = 1.25;
        o["Gravity"]  = std::move( gravity );
        o["FogColor"] = "grey";
        return rfl::Generic( o );
    }

    std::optional<double> GravityOf( const std::optional<rfl::Generic>& settings )
    {
        if ( !settings.has_value() )
            return std::nullopt;
        const auto fields = settings.value().to_object();
        if ( !fields.has_value() )
            return std::nullopt;
        const auto g = fields.value().get( "Gravity" );
        if ( !g.has_value() )
            return std::nullopt;
        if ( const auto d = g.value().to_double(); d.has_value() )
            return d.value();
        if ( const auto i = g.value().to_int64(); i.has_value() )
            return static_cast<double>( i.value() );
        return std::nullopt;
    }

    std::vector<std::string> KeysOf( const std::optional<rfl::Generic>& settings )
    {
        std::vector<std::string> keys;
        if ( !settings.has_value() )
            return keys;
        const auto fields = settings.value().to_object();
        if ( !fields.has_value() )
            return keys;
        for ( const auto& [key, value] : fields.value() )
            keys.push_back( key );
        return keys;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // `Scenes/Autosave/` is the editor's gitignored crash-recovery copy, never migrated. A recursive sweep
    // would otherwise pass or fail on whatever the last editor session left behind — green in CI, red on
    // any machine that has ever run the editor.
    bool IsShippedScene( const std::filesystem::path& p )
    {
        for ( const auto& part : p )
            if ( part == "Autosave" )
                return false;
        return true;
    }
} // namespace

// ── The pure function ──────────────────────────────────────────────────────────────────────────────

TEST( SceneGravityUnitsMigration, TheMetreEraEarthBecomesCentimetres )
{
    // The exact bits 9.81 has after a float round-trip, which is what the 39 scenes actually contained.
    auto settings = SettingsWith( 9.8100004196167 );

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_TRUE( report.Found );
    EXPECT_TRUE( report.Scaled );
    EXPECT_FALSE( report.Unrecognised );
    ASSERT_TRUE( GravityOf( settings ).has_value() );
    EXPECT_DOUBLE_EQ( 981.0, GravityOf( settings ).value() );
}

TEST( SceneGravityUnitsMigration, TheRoundingOfTheEarlierPassIsDropped )
{
    // 9.8100004196167 x 100 exactly: a file that WAS migrated, carrying the arithmetic instead of a value.
    auto settings = SettingsWith( 981.0000419616699 );

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_TRUE( report.Found );
    EXPECT_FALSE( report.Scaled ) << "it was already centimetres; scaling it again would give 98100";
    EXPECT_TRUE( report.Tidied );
    EXPECT_DOUBLE_EQ( 981.0, GravityOf( settings ).value() );
}

TEST( SceneGravityUnitsMigration, AnAlreadyCleanValueIsNeitherScaledNorReportedAsTidied )
{
    auto settings = SettingsWith( 981.0 );

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_TRUE( report.Found );
    EXPECT_FALSE( report.Scaled );
    EXPECT_FALSE( report.Tidied );
    EXPECT_FALSE( report.Unrecognised );
    EXPECT_DOUBLE_EQ( 981.0, GravityOf( settings ).value() );
}

// The assertion that keeps this migration honest. A threshold ("anything under 100 is metres") would
// silently turn a deliberately weak gravity into a hundredfold one, which is the substitution §1.4 forbids.
TEST( SceneGravityUnitsMigration, AnUnrecognisedValueIsLeftAloneAndNamed )
{
    auto settings = SettingsWith( 250.0 ); // a low-gravity level, authored on purpose

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_TRUE( report.Found );
    EXPECT_TRUE( report.Unrecognised );
    EXPECT_FALSE( report.Scaled );
    EXPECT_DOUBLE_EQ( 250.0, GravityOf( settings ).value() ) << "it must survive the migration untouched";
    EXPECT_FLOAT_EQ( 250.0F, report.Before );
    EXPECT_FLOAT_EQ( 250.0F, report.After );
}

// Half of Earth in METRES. Also unrecognised — and this is the case the report exists for, because it is
// genuinely wrong in the file and only a human can say so.
TEST( SceneGravityUnitsMigration, AMetreEraValueThatIsNotEarthIsAlsoRefusedRatherThanGuessed )
{
    auto settings = SettingsWith( 4.905 );

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_TRUE( report.Unrecognised );
    EXPECT_DOUBLE_EQ( 4.905, GravityOf( settings ).value() );
}

// ── The shapes a hand-edited file has ──────────────────────────────────────────────────────────────

TEST( SceneGravityUnitsMigration, AnIntegerSpellingIsANumber )
{
    // reflect-cpp parses `"Gravity":981` as int64, and to_double() refuses it. Reading the value with
    // to_double alone would report a hand-edited scene as "not a number" and leave it unmigrated.
    auto settings = SettingsWith( static_cast<std::int64_t>( 981 ) );

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_TRUE( report.Found ) << "an integer 981 is a gravity, not a broken field";
    EXPECT_FALSE( report.Unrecognised );
    EXPECT_DOUBLE_EQ( 981.0, GravityOf( settings ).value() );
}

TEST( SceneGravityUnitsMigration, AnIntegerMetreEraValueIsScaledToo )
{
    auto settings = SettingsWith( static_cast<std::int64_t>( 10 ) ); // outside the tolerance of 9.81

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_TRUE( report.Found );
    EXPECT_TRUE( report.Unrecognised ) << "10 is not Earth in either unit, so it must not be guessed at";
    EXPECT_DOUBLE_EQ( 10.0, GravityOf( settings ).value() );
}

TEST( SceneGravityUnitsMigration, ASettingsBlockWithoutGravityIsUntouched )
{
    rfl::Generic::Object o;
    o["Exposure"]                        = 1.25;
    std::optional<rfl::Generic> settings = rfl::Generic( o );

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_FALSE( report.Found );
    EXPECT_EQ( std::vector<std::string>{ "Exposure" }, KeysOf( settings ) );
}

TEST( SceneGravityUnitsMigration, AnAbsentSettingsBlockIsNotAnError )
{
    std::optional<rfl::Generic> settings;

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_FALSE( report.Found );
    EXPECT_FALSE( settings.has_value() ) << "nothing may be invented for a scene that states no settings";
}

TEST( SceneGravityUnitsMigration, ANonNumericGravityIsLeftExactlyAsItIs )
{
    auto settings = SettingsWith( std::string( "heavy" ) );

    const auto report = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_FALSE( report.Found ) << "a string is not a gravity that was found, it is a field to report";
    const auto fields = settings.value().to_object();
    ASSERT_TRUE( fields.has_value() );
    EXPECT_EQ( "heavy", fields.value().get( "Gravity" ).value().to_string().value() );
}

TEST( SceneGravityUnitsMigration, TheRestOfTheSettingsBlockSurvivesInOrder )
{
    auto settings = SettingsWith( 9.8100004196167 );

    Migration::MigrateGravityUnitsV8ToV9( settings );

    const std::vector<std::string> expected{ "Exposure", "Gravity", "FogColor" };
    EXPECT_EQ( expected, KeysOf( settings ) ) << "the block is rebuilt, so key order is a thing to assert";
}

TEST( SceneGravityUnitsMigration, RunningItTwiceChangesNothingTheSecondTime )
{
    auto settings = SettingsWith( 9.8100004196167 );

    const auto first = Migration::MigrateGravityUnitsV8ToV9( settings );
    ASSERT_TRUE( first.Scaled );
    const std::string afterFirst = rfl::json::write( settings.value() );

    const auto second = Migration::MigrateGravityUnitsV8ToV9( settings );

    EXPECT_FALSE( second.Scaled ) << "the second run must find centimetres and leave them alone";
    EXPECT_EQ( afterFirst, rfl::json::write( settings.value() ) );
}

// ── The relation, and the class ────────────────────────────────────────────────────────────────────

// Two statements of one number that must never drift: what the migration writes into every file, and what
// the component defaults to for a scene that states nothing. The physics world is handed one of these two
// depending on whether the scene names a gravity, so a disagreement is a world that falls differently
// depending on which path it took.
TEST( SceneGravityUnitsMigration, TheMigratedValueAndTheComponentDefaultAreOneNumber )
{
    auto settings = SettingsWith( 9.8100004196167 );
    Migration::MigrateGravityUnitsV8ToV9( settings );

    const Core::SceneSettings defaults;
    ASSERT_TRUE( GravityOf( settings ).has_value() );
    EXPECT_FLOAT_EQ( defaults.Gravity, static_cast<float>( GravityOf( settings ).value() ) );
}

// The class-level assertion: whatever else changes, no shipped scene may carry a metre-era gravity again.
// This is the one test of the file that would catch the defect coming back through some other door — a new
// scene copied from an old template, or a migration step accidentally gated on the head version.
TEST( SceneGravityUnitsMigration, NoShippedSceneCarriesAMetreEraGravity )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the test's working directory";

    const std::filesystem::path scenes = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( scenes ) ) << scenes.string();

    int checked = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( scenes ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        if ( !IsShippedScene( entry.path() ) )
            continue;

        std::ifstream     in( entry.path(), std::ios::binary );
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto parsed = rfl::json::read<Desert::Migration::SceneSerialized>( buffer.str() );
        ASSERT_TRUE( parsed ) << entry.path().string() << " did not parse";

        std::optional<rfl::Generic> settings = parsed.value().Settings;
        const auto                  gravity  = GravityOf( settings );
        if ( !gravity.has_value() )
            continue; // states no gravity; the component default applies and that is centimetres

        ++checked;
        EXPECT_GT( gravity.value(), 100.0 )
             << entry.path().string() << " states a gravity of " << gravity.value()
             << ", which is metre-era: one world unit is one CENTIMETRE, so this scene falls a hundred "
                "times too slowly now that the physics world actually reads the field";
    }

    EXPECT_GT( checked, 0 ) << "no scene stated a gravity at all - this test would pass vacuously";
}

// Gated on its own integer, not on the head. Gating every step on kSceneVersion is what would send an
// already-migrated file back through an earlier step the moment the head moved.
TEST( SceneGravityUnitsMigration, TheStepHasItsOwnVersionAndItIsTheHead )
{
    EXPECT_EQ( 9, Migration::kSceneVersionGravityUnits );
    // It was the newest step when it was written and is not any more (v10 folds the UI interaction flags
    // into two enums). What has to stay true is that it is BELOW the head — a step at or above the
    // generation the loader requires would stamp files the loader refuses.
    EXPECT_LT( Migration::kSceneVersionGravityUnits, Core::kSceneVersion );
    EXPECT_GT( Migration::kSceneVersionGravityUnits, Migration::kSceneVersionMaterialPath );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
