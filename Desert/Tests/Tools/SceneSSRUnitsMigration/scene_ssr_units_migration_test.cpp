// The v10 -> v11 SSR-units migration, and the relation it exists to make permanent.
//
// `SceneSettings::SSRMaxDistance` was authored under a metre-scale slider (Range 1..200, default 40) and
// consumed by SSR.shader as a WORLD distance - and a world unit is a centimetre. So every reflection ray
// died forty CENTIMETRES out and the slider could not express anything past two metres. The same
// disagreement, in the same direction, as the metre-era gravity, the grass interaction radius and the
// shadow distance before it: both ends correct in isolation, the relation between them wrong by x100.
//
// Unlike gravity, this step scales EVERY finite value instead of recognising known spellings, and the
// header documents why: a v10 file has no second population. Gravity had metre-era and centimetre-era
// values under one stamp; SSRMaxDistance could only ever be authored under the metre-scale slider, so
// inside the version gate x100 is a restatement of the author's magnitude, not a guess. The flip side is
// asserted here too - the function is NOT idempotent on its own and relies on the gate.
//
// What is asserted:
//
//   1. The pure function scales a finite value by exactly 100 and reports before/after.
//   2. It survives the shapes a hand-edited file has: an integer, a missing key, a wrong type, no
//      Settings block at all - and leaves the rest of the block untouched and in order.
//   3. It is gated on its OWN version integer, and a file at the head does not run it.
//   4. THE RELATION: what the migration writes for the old default and what SceneSettings defaults to
//      are one number, and no shipped scene carries a metre-era value.

#include <SceneMigration.hpp>

#include <Engine/Core/SceneSettings.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Desert;

namespace
{
    // A Settings block with an SSRMaxDistance of the given spelling, plus two neighbours so the test can
    // prove the rest of the block survives untouched and in order.
    std::optional<rfl::Generic> SettingsWith( rfl::Generic distance )
    {
        rfl::Generic::Object o;
        o["SSRIntensity"]   = 1.0;
        o["SSRMaxDistance"] = std::move( distance );
        o["EnableSSR"]      = false;
        return rfl::Generic( o );
    }

    std::optional<double> DistanceOf( const std::optional<rfl::Generic>& settings )
    {
        if ( !settings.has_value() )
            return std::nullopt;
        const auto fields = settings.value().to_object();
        if ( !fields.has_value() )
            return std::nullopt;
        const auto d = fields.value().get( "SSRMaxDistance" );
        if ( !d.has_value() )
            return std::nullopt;
        if ( const auto v = d.value().to_double(); v.has_value() )
            return v.value();
        if ( const auto i = d.value().to_int64(); i.has_value() )
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
    // would otherwise pass or fail on whatever the last editor session left behind - green in CI, red on
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

TEST( SceneSSRUnitsMigration, TheMetreEraDefaultBecomesCentimetres )
{
    auto settings = SettingsWith( 40.0 ); // the default every shipped scene actually carried

    const auto report = Migration::MigrateSSRUnitsV10ToV11( settings );

    EXPECT_TRUE( report.Found );
    EXPECT_TRUE( report.Scaled );
    EXPECT_FLOAT_EQ( 40.0F, report.Before );
    EXPECT_FLOAT_EQ( 4000.0F, report.After );
    ASSERT_TRUE( DistanceOf( settings ).has_value() );
    EXPECT_DOUBLE_EQ( 4000.0, DistanceOf( settings ).value() );
}

TEST( SceneSSRUnitsMigration, ANonDefaultValueIsScaledByTheSameHundred )
{
    // 200 was the old slider's ceiling - the strongest "I wanted more reach" a v10 author could say.
    auto settings = SettingsWith( 200.0 );

    const auto report = Migration::MigrateSSRUnitsV10ToV11( settings );

    EXPECT_TRUE( report.Scaled );
    EXPECT_DOUBLE_EQ( 20000.0, DistanceOf( settings ).value() );
}

// ── The shapes a hand-edited file has ──────────────────────────────────────────────────────────────

TEST( SceneSSRUnitsMigration, AnIntegerSpellingIsANumber )
{
    // reflect-cpp parses `"SSRMaxDistance":40` as int64, and to_double() refuses it. Reading the value
    // with to_double alone would report a hand-edited scene as "not a number" and leave it unmigrated.
    auto settings = SettingsWith( static_cast<std::int64_t>( 40 ) );

    const auto report = Migration::MigrateSSRUnitsV10ToV11( settings );

    EXPECT_TRUE( report.Scaled ) << "an integer 40 is a distance, not a broken field";
    EXPECT_DOUBLE_EQ( 4000.0, DistanceOf( settings ).value() );
}

TEST( SceneSSRUnitsMigration, ASettingsBlockWithoutTheKeyIsUntouched )
{
    rfl::Generic::Object o;
    o["SSRIntensity"]                    = 1.0;
    std::optional<rfl::Generic> settings = rfl::Generic( o );

    const auto report = Migration::MigrateSSRUnitsV10ToV11( settings );

    EXPECT_FALSE( report.Found );
    EXPECT_EQ( std::vector<std::string>{ "SSRIntensity" }, KeysOf( settings ) );
}

TEST( SceneSSRUnitsMigration, AnAbsentSettingsBlockIsNotAnError )
{
    std::optional<rfl::Generic> settings;

    const auto report = Migration::MigrateSSRUnitsV10ToV11( settings );

    EXPECT_FALSE( report.Found );
    EXPECT_FALSE( settings.has_value() ) << "nothing may be invented for a scene that states no settings";
}

TEST( SceneSSRUnitsMigration, ANonNumericValueIsLeftExactlyAsItIs )
{
    auto settings = SettingsWith( std::string( "far" ) );

    const auto report = Migration::MigrateSSRUnitsV10ToV11( settings );

    EXPECT_FALSE( report.Found ) << "a string is not a distance that was found, it is a field to report";
    const auto fields = settings.value().to_object();
    ASSERT_TRUE( fields.has_value() );
    EXPECT_EQ( "far", fields.value().get( "SSRMaxDistance" ).value().to_string().value() );
}

TEST( SceneSSRUnitsMigration, TheRestOfTheSettingsBlockSurvivesInOrder )
{
    auto settings = SettingsWith( 40.0 );

    Migration::MigrateSSRUnitsV10ToV11( settings );

    const std::vector<std::string> expected{ "SSRIntensity", "SSRMaxDistance", "EnableSSR" };
    EXPECT_EQ( expected, KeysOf( settings ) ) << "the block is rebuilt, so key order is a thing to assert";
}

// ── The gate the function's own honesty depends on ─────────────────────────────────────────────────

// Unlike gravity, the pure function is NOT idempotent (the header says why: no second population to
// recognise). That makes the version gate load-bearing, so the gate is what gets asserted.
TEST( SceneSSRUnitsMigration, AFileAtTheHeadDoesNotRunTheStep )
{
    Desert::Migration::SceneSerialized scene;
    scene.SceneVersion = Core::kSceneVersion;
    scene.UnitVersion  = Core::kUnitVersion;
    scene.Settings     = SettingsWith( 4000.0 );

    EXPECT_FALSE( Migration::MigrateScene( scene, "Resources/Assets" ).SSRUnitsRaised );
    EXPECT_DOUBLE_EQ( 4000.0, DistanceOf( scene.Settings ).value() )
         << "a centimetre value under a current stamp must never see a second x100";
}

TEST( SceneSSRUnitsMigration, AV10FileRunsTheStepOnceAndComesOutStamped )
{
    Desert::Migration::SceneSerialized scene;
    scene.SceneVersion = 10;
    scene.UnitVersion  = Core::kUnitVersion;
    scene.Settings     = SettingsWith( 40.0 );

    const auto report = Migration::MigrateScene( scene, "Resources/Assets" );

    EXPECT_TRUE( report.SSRUnitsRaised );
    EXPECT_DOUBLE_EQ( 4000.0, DistanceOf( scene.Settings ).value() );
    ASSERT_TRUE( scene.Header.has_value() );
    EXPECT_EQ( Core::kSceneVersion,
               Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ) )
         << "an unstamped result is a file the next run would scale again";
}

// NOT THE HEAD ANY MORE, and that is why this test still exists rather than being deleted. It asserted
// two things at once — "SSR is step 11" and "step 11 is what the loader requires" — and O1 added step 12
// above it, which turned the second into a claim about the newest step wearing this step's name. The
// first half is this step's own and stays; the second belongs to whichever step is last, is enforced at
// COMPILE time beside kSceneVersionCloudMaterial in SceneMigration.hpp, and is asserted at run time by
// SceneCloudMaterialMigration. What is left here is the ORDER, which is this step's business: SSR sits
// above UI visibility and below the cloud material, and a step inserted out of order would run against a
// tree that has not been through its predecessor.
TEST( SceneSSRUnitsMigration, TheStepHasItsOwnVersionAndItSitsBetweenItsNeighbours )
{
    EXPECT_EQ( 11, Migration::kSceneVersionSSRUnits );
    EXPECT_GT( Migration::kSceneVersionSSRUnits, Migration::kSceneVersionUIVisibility );
    EXPECT_LT( Migration::kSceneVersionSSRUnits, Migration::kSceneVersionCloudMaterial )
         << "the cloud material step is the one above this; if it stopped being, the head assertion that "
            "moved out of this test moved to the wrong suite";
    EXPECT_LE( Migration::kSceneVersionSSRUnits, Core::kSceneVersion );
}

// ── The relation, and the class ────────────────────────────────────────────────────────────────────

// Two statements of one number that must never drift: what the migration writes into a file that carried
// the old default, and what the component defaults to for a scene that states nothing. The renderer is
// handed one of these two depending on whether the scene names a distance, so a disagreement is a
// reflection that reaches differently depending on which path it took.
TEST( SceneSSRUnitsMigration, TheMigratedDefaultAndTheComponentDefaultAreOneNumber )
{
    auto settings = SettingsWith( 40.0 );
    Migration::MigrateSSRUnitsV10ToV11( settings );

    const Core::SceneSettings defaults;
    ASSERT_TRUE( DistanceOf( settings ).has_value() );
    EXPECT_FLOAT_EQ( defaults.SSRMaxDistance, static_cast<float>( DistanceOf( settings ).value() ) );
}

// The class-level assertion: whatever else changes, no shipped scene may carry a metre-era SSR distance
// again. 100 is the new slider's own floor (1 m), and every metre-era value the old slider could produce
// (1..200, default 40) lies below it after the census confirmed only 40.0 existed in the tree.
TEST( SceneSSRUnitsMigration, NoShippedSceneCarriesAMetreEraDistance )
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
        const auto                  distance = DistanceOf( settings );
        if ( !distance.has_value() )
            continue; // states no distance; the component default applies and that is centimetres

        ++checked;
        EXPECT_GE( distance.value(), 100.0 )
             << entry.path().string() << " states an SSR max distance of " << distance.value()
             << ", below the slider's own 1 m floor: one world unit is one CENTIMETRE, so this is a "
                "metre-era value and its reflection rays die within arm's reach";
    }

    EXPECT_GT( checked, 0 ) << "no scene stated an SSR distance at all - this test would pass vacuously";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
