// The v12 -> v13 debug-view migration: the ten keys that said what a VIEWPORT was drawing leave the level
// file for good.
//
// WHAT WAS ACTUALLY WRONG, measured over the 83 .desce files in this tree before the step existed:
// `ShowColliders` defaulted to TRUE and was written `true` in 55 of the 73 scenes that stated it, so green
// physics wireframes travelled through git into everybody's viewport; `ShowGrid` was stated by 77 scenes
// and was `false` in 72, overriding whatever grid preference the person opening the file had. The other
// six were uniform — noise in the file, but noise that one bad save would have turned into a level that
// opens in wireframe for everyone. None of them is a property of the world.
//
// This step therefore CARRIES NOTHING. There is nowhere to carry to: Graphic::DebugViewState lives on the
// SceneRenderer, one per view, and is serialized nowhere; the editor's copy is a user preference. Turning
// 80 scenes' worth of one-time view state into a user preference would be the same confusion in a new
// file, so every flag simply defaults off and the user turns on what they want once.
//
// What is asserted:
//
//   1. The pure function removes exactly the ten keys and nothing else, and leaves the rest of the block
//      untouched and in order.
//   2. It survives the shapes a hand-edited file has: a key of the wrong JSON type, a missing key, a
//      Settings block that is not an object, no Settings block at all.
//   3. It is idempotent, and gated on its own version integer through MigrateScene.
//   4. THE RELATION, and it is the reason this suite is worth more than its assertions: the set the
//      migration removes is the set Graphic::DebugViewState owns and the set Core::SceneSettings must not
//      have. Two of the three are checked here against the live reflection table; the third
//      (DebugViewState's own declaration) is checked by Desert/Tests/Engine/SceneDebugFields, which reads
//      all three as text.

#include <SceneMigration.hpp>

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

    bool Has( const std::optional<rfl::Generic>& settings, const std::string& key )
    {
        const std::vector<std::string> keys = KeysOf( settings );
        return std::find( keys.begin(), keys.end(), key ) != keys.end();
    }

    // A Settings block exactly as the repository's scenes carried it: every debug key at the value that
    // was actually most common on disk, plus three real level properties around them so the test can prove
    // the rest of the block survives untouched.
    std::optional<rfl::Generic> SettingsAsShipped()
    {
        rfl::Generic::Object o;
        o["Exposure"]             = 0.22;
        o["ShowGrid"]             = false;
        o["ShowColliders"]        = true; // the 55-of-80 value
        o["ShowBoundingBoxes"]    = false;
        o["BoundingBoxColor"]     = rfl::Generic::Array{ 0.25, 0.95, 0.35 };
        o["BoundingBoxLineWidth"] = 1.5;
        o["WireframeMode"]        = false;
        o["ShadowDebug"]          = 0;
        o["DeferredDebug"]        = 0;
        o["Gravity"]              = 981.0;
        o["MeshLOD"]              = true;
        return rfl::Generic( o );
    }
} // namespace

// ── The pure function ──────────────────────────────────────────────────────────────────────────────

TEST( SceneDebugViewMigration, EveryDebugKeyLeavesAndEveryLevelPropertyStays )
{
    auto settings = SettingsAsShipped();

    const auto report = Migration::MigrateDebugViewV12ToV13( settings );

    EXPECT_EQ( report.KeysRemoved, 8 ); // the eight a v12 file could actually state
    EXPECT_EQ( KeysOf( settings ), ( std::vector<std::string>{ "Exposure", "Gravity", "MeshLOD" } ) );
}

// NAMED WITH THEIR VALUES, not counted. "8 keys removed" does not tell the person running the tool that
// the collider wireframes they are used to seeing are gone because the file stopped deciding it.
TEST( SceneDebugViewMigration, WhatWasRemovedIsReportedWithTheValueTheFileStated )
{
    auto settings = SettingsAsShipped();

    const auto report = Migration::MigrateDebugViewV12ToV13( settings );

    EXPECT_NE( std::find( report.RemovedNames.begin(), report.RemovedNames.end(), "ShowColliders=true" ),
               report.RemovedNames.end() );
    EXPECT_NE( std::find( report.RemovedNames.begin(), report.RemovedNames.end(), "ShowGrid=false" ),
               report.RemovedNames.end() );
    EXPECT_EQ( report.RemovedNames.size(), static_cast<std::size_t>( report.KeysRemoved ) );
}

// A KEY OF THE WRONG TYPE GOES TOO, and that is not the usual answer for this project's migrations: the
// gravity and SSR steps REFUSE a value they cannot read, because they rewrite a field that still exists
// and guessing at it would be a silent substitution. Nothing is being guessed at here — the field does not
// exist any more, so there is no type for `"ShowColliders": "yes"` to be wrong for. It is still named in
// the report, which is what §1.4 actually asks for.
TEST( SceneDebugViewMigration, AKeyOfTheWrongTypeIsRemovedAndNamedRatherThanKept )
{
    rfl::Generic::Object o;
    o["ShowColliders"]                   = std::string( "yes" );
    o["Exposure"]                        = 1.0;
    std::optional<rfl::Generic> settings = rfl::Generic( o );

    const auto report = Migration::MigrateDebugViewV12ToV13( settings );

    EXPECT_EQ( report.KeysRemoved, 1 );
    EXPECT_FALSE( Has( settings, "ShowColliders" ) );
    EXPECT_TRUE( Has( settings, "Exposure" ) );
    EXPECT_EQ( report.RemovedNames, ( std::vector<std::string>{ "ShowColliders=\"yes\"" } ) );
}

TEST( SceneDebugViewMigration, ASceneWithNoSettingsBlockIsUntouchedAndReportsZero )
{
    std::optional<rfl::Generic> settings; // three UI_VisibilityStack scenes really are like this

    const auto report = Migration::MigrateDebugViewV12ToV13( settings );

    EXPECT_EQ( report.KeysRemoved, 0 );
    EXPECT_TRUE( report.RemovedNames.empty() );
    EXPECT_FALSE( settings.has_value() );
}

// A Settings block that is not an object at all: warned about and left EXACTLY as it is, because there is
// nothing here to remove keys from and replacing it with an empty object would delete a level's settings
// on the strength of a parse failure.
TEST( SceneDebugViewMigration, ASettingsBlockThatIsNotAnObjectIsLeftAlone )
{
    std::optional<rfl::Generic> settings = rfl::Generic( std::string( "not an object" ) );

    const auto report = Migration::MigrateDebugViewV12ToV13( settings );

    EXPECT_EQ( report.KeysRemoved, 0 );
    ASSERT_TRUE( settings.has_value() );
    ASSERT_TRUE( settings.value().to_string().has_value() );
    EXPECT_EQ( settings.value().to_string().value(), "not an object" );
}

TEST( SceneDebugViewMigration, ASecondRunRemovesNothingAndChangesNothing )
{
    auto settings = SettingsAsShipped();
    (void)Migration::MigrateDebugViewV12ToV13( settings );
    const std::vector<std::string> afterFirst = KeysOf( settings );

    const auto second = Migration::MigrateDebugViewV12ToV13( settings );

    EXPECT_EQ( second.KeysRemoved, 0 );
    EXPECT_EQ( KeysOf( settings ), afterFirst );
}

// ── The gate ───────────────────────────────────────────────────────────────────────────────────────

TEST( SceneDebugViewMigration, MigrateSceneRunsTheStepBelowTheHeadAndStampsIt )
{
    Desert::Migration::SceneSerialized scene;
    scene.SceneName    = "Fixture";
    scene.SceneVersion = Migration::kSceneVersionDebugView - 1;
    scene.UnitVersion  = Migration::kUnitVersion;
    scene.Settings     = SettingsAsShipped();

    const auto report = Migration::MigrateScene( scene );

    EXPECT_TRUE( report.DebugViewRaised );
    EXPECT_EQ( report.DebugView.KeysRemoved, 8 );
    EXPECT_FALSE( Has( scene.Settings, "ShowColliders" ) );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Migration::kSceneVersion );
}

TEST( SceneDebugViewMigration, AFileAlreadyAtTheHeadDoesNotRunTheStep )
{
    Desert::Migration::SceneSerialized scene;
    scene.SceneName    = "Fixture";
    scene.SceneVersion = Migration::kSceneVersion;
    scene.UnitVersion  = Migration::kUnitVersion;
    scene.Settings     = SettingsAsShipped(); // hand-edited back in; the gate must not care

    const auto report = Migration::MigrateScene( scene );

    EXPECT_FALSE( report.DebugViewRaised );
    EXPECT_FALSE( report.Changed() );
    EXPECT_TRUE( Has( scene.Settings, "ShowColliders" ) );
}

// THE HEAD ASSERTION HAS MOVED ON, to SceneRetiredKeysMigration — v14 is the head since K11. What stays
// here is this step's own business: its number, and that it sits directly above the cloud material step.
// The head half is deliberately in exactly one suite at a time, and it is the run-time counterpart of the
// static_assert in SceneMigration.hpp.
TEST( SceneDebugViewMigration, ThisStepSitsAboveItsPredecessorAndIsNoLongerTheHead )
{
    EXPECT_EQ( 13, Migration::kSceneVersionDebugView );
    EXPECT_GT( Migration::kSceneVersionDebugView, Migration::kSceneVersionCloudMaterial );
    EXPECT_LT( Migration::kSceneVersionDebugView, Core::kSceneVersion )
         << "this is the head step again; the head assertion belongs back in this suite";
}

// ── The relation ───────────────────────────────────────────────────────────────────────────────────

// The set this migration removes and the set Core::SceneSettings still exposes must be DISJOINT. Not "the
// eight names are gone" — that is a list, and a list goes stale. This asks the live reflection table,
// which is what the serializer writes from, so a field re-added to SceneSettings under any of these names
// fails here as well as in Desert/Tests/Engine/SceneDebugFields.
TEST( SceneDebugViewMigration, NothingTheStepStripsIsStillAReflectedSceneSetting )
{
    const auto* info = Reflection::ReflectionRegistry::Get().Find( "SceneSettings" );
    ASSERT_NE( info, nullptr );

    for ( const char* key : Migration::kDebugViewKeys )
    {
        const bool present = std::any_of( info->Fields.begin(), info->Fields.end(),
                                          [key]( const Reflection::FieldInfo& f ) { return f.Name == key; } );
        EXPECT_FALSE( present ) << "SceneSettings::" << key
                                << " is reflected again, so every scene file states it again - while this "
                                   "migration is still busy removing it from every scene file.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
