// The v9 -> v10 UI-visibility migration: two booleans on `UILayout` become one enum beside a second one.
//
// WHAT MOVED, AND WHY A MIGRATION IS OWED AT ALL. `UILayoutData` carried `Interactable` and
// `RaycastTarget`, each a per-element answer to a question the WALK asks about a whole sub-tree. Both are
// gone; `HitTest` answers both, and it is inherited. The file therefore has to be restated, and the engine
// no longer migrates anything at load — so this function and Tools/SceneMigrator are the whole conversion.
//
// THE MAPPING IS THE POINT, and it is asserted one row at a time below:
//
//   RaycastTarget = false  ->  ChildrenOnly (1)   identical behaviour; UE calls it SelfHitTestInvisible
//   Interactable  = false  ->  Blocking     (2)   same element behaviour, now inherited by the sub-tree
//   both false             ->  ChildrenOnly (1)   transparent subsumes inert; Blocking would ADD blocking
//   either true            ->  nothing written    an absent key IS UIHitTest::All, the C++ default
//
// `Visibility` is never written by this step and there is a test for that: the old format could not say
// anything but "visible" about an element, so any value would be invented.
//
// PURE, and this project failing to link without a renderer is the proof: the migration is a function over
// the parsed property tree, with no GPU, no filesystem and no globals.

#include <SceneMigration.hpp>

// The enum the written integers have to agree with. Naming the enumerators here rather than repeating 1
// and 2 is what keeps this suite true if they are ever reordered — the migration writes numbers, and a
// number is exactly the thing nobody can check against an enum six months later.
#include <Engine/ECS/Components.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert;

namespace
{
    // The keys a fixture states, in the order it states them. A vector rather than an rfl::Generic::Object
    // because the ORDER matters to two of the assertions below and reflect-cpp's object takes no
    // initializer list of pairs.
    using Keys = std::vector<std::pair<std::string, rfl::Generic>>;

    rfl::Generic::Object ObjectOf( const Keys& keys )
    {
        rfl::Generic::Object object;
        for ( const auto& [key, value] : keys )
            object[key] = value;
        return object;
    }

    // One entity carrying a "UILayout" payload with the given keys, plus two neighbours so the test can
    // prove the rest of the payload survives untouched and in order.
    Assets::EntityData ElementWith( const Keys& layout, const char* tag = "Element" )
    {
        Assets::EntityData entity;
        entity.Tag                  = tag;
        rfl::Generic::Object padded = {};
        padded["AnchorMin"] = rfl::Generic( rfl::Generic::Array{ rfl::Generic( 0.0 ), rfl::Generic( 0.0 ) } );
        for ( const auto& [key, value] : layout )
            padded[key] = value;
        padded["ClipContents"]        = rfl::Generic( false );
        entity.Components["UILayout"] = rfl::Generic( padded );
        return entity;
    }

    rfl::Generic::Object LayoutOf( const Assets::EntityData& entity )
    {
        const auto found = entity.Components.get( "UILayout" );
        if ( !found.has_value() )
            return {};
        return found.value().to_object().value_or( rfl::Generic::Object{} );
    }

    bool HasKey( const rfl::Generic::Object& object, const std::string& key )
    {
        for ( const auto& [k, v] : object )
            if ( k == key )
                return true;
        return false;
    }

    std::optional<int> IntAt( const rfl::Generic::Object& object, const std::string& key )
    {
        const auto found = object.get( key );
        if ( !found.has_value() )
            return std::nullopt;
        if ( const auto i = found.value().to_int64(); i.has_value() )
            return static_cast<int>( i.value() );
        return std::nullopt;
    }

    std::vector<std::string> KeysOf( const rfl::Generic::Object& object )
    {
        std::vector<std::string> keys;
        for ( const auto& [k, v] : object )
            keys.push_back( k );
        return keys;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // `Scenes/Autosave/` is the editor's gitignored crash-recovery copy and is never migrated. Sweeping it
    // would make this suite pass or fail on whatever the last editor session left behind.
    bool IsShippedScene( const std::filesystem::path& p )
    {
        for ( const auto& part : p )
            if ( part == "Autosave" )
                return false;
        return true;
    }
} // namespace

// ── The mapping, one row at a time ─────────────────────────────────────────────────────────────────

TEST( SceneUIVisibilityMigration, RaycastTargetOffBecomesChildrenOnly )
{
    std::vector<Assets::EntityData> entities{ ElementWith( Keys{ { "RaycastTarget", rfl::Generic( false ) } } ) };

    const auto report = Migration::MigrateUIVisibilityV9ToV10( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.FlagsDropped, 1 );
    EXPECT_EQ( report.HitTestSet, 1 );
    EXPECT_TRUE( report.BrokenNames.empty() );

    const auto& layout = LayoutOf( entities[0] );
    EXPECT_FALSE( HasKey( layout, "RaycastTarget" ) ) << "the field is gone from the component; DC 4.1 says "
                                                         "the key goes with it";
    ASSERT_TRUE( IntAt( layout, "HitTest" ).has_value() );
    EXPECT_EQ( *IntAt( layout, "HitTest" ), static_cast<int>( ECS::UIHitTest::ChildrenOnly ) );
}

TEST( SceneUIVisibilityMigration, InteractableOffBecomesBlocking )
{
    std::vector<Assets::EntityData> entities{ ElementWith( Keys{ { "Interactable", rfl::Generic( false ) } } ) };

    const auto report = Migration::MigrateUIVisibilityV9ToV10( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.HitTestSet, 1 );
    const auto& layout = LayoutOf( entities[0] );
    EXPECT_FALSE( HasKey( layout, "Interactable" ) );
    ASSERT_TRUE( IntAt( layout, "HitTest" ).has_value() );
    EXPECT_EQ( *IntAt( layout, "HitTest" ), static_cast<int>( ECS::UIHitTest::Blocking ) );
}

// BOTH OFF IS NOT THE SUM OF THE TWO. An element the pointer cannot see was never pressed anyway, so the
// raycast answer already covers the interactable one — while Blocking would make the element STOP clicks
// it used to let through, which is behaviour the file never asked for. Asserted in both key orders,
// because the migration walks the payload in the order the file happens to state it.
TEST( SceneUIVisibilityMigration, BothOffIsChildrenOnlyWhicheverOrderTheFileStatesThemIn )
{
    for ( const bool raycastFirst : { true, false } )
    {
        const Keys layout =
             raycastFirst
                  ? Keys{ { "RaycastTarget", rfl::Generic( false ) }, { "Interactable", rfl::Generic( false ) } }
                  : Keys{ { "Interactable", rfl::Generic( false ) }, { "RaycastTarget", rfl::Generic( false ) } };

        std::vector<Assets::EntityData> entities{ ElementWith( layout ) };
        const auto                      report = Migration::MigrateUIVisibilityV9ToV10( entities );

        EXPECT_EQ( report.FlagsDropped, 2 );
        EXPECT_EQ( report.HitTestSet, 1 ) << "two flags produced two Hit Test keys";
        ASSERT_TRUE( IntAt( LayoutOf( entities[0] ), "HitTest" ).has_value() );
        EXPECT_EQ( *IntAt( LayoutOf( entities[0] ), "HitTest" ), static_cast<int>( ECS::UIHitTest::ChildrenOnly ) )
             << "raycast-first was " << raycastFirst;
    }
}

// THE DEFAULT IS SPELT BY ABSENCE. A file that states `true` states the default, and writing HitTest = All
// for it would put a value in every scene that carries no information — and then the day the default moves
// those scenes would be pinned to the old one for a reason nobody could find.
TEST( SceneUIVisibilityMigration, FlagsThatWereOnLeaveNoKeyBehind )
{
    std::vector<Assets::EntityData> entities{ ElementWith(
         Keys{ { "Interactable", rfl::Generic( true ) }, { "RaycastTarget", rfl::Generic( true ) } } ) };

    const auto report = Migration::MigrateUIVisibilityV9ToV10( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.FlagsDropped, 2 );
    EXPECT_EQ( report.HitTestSet, 0 );

    const auto& layout = LayoutOf( entities[0] );
    EXPECT_FALSE( HasKey( layout, "HitTest" ) )
         << "an absent key is how the serializer spells the C++ default, and All IS the default";
    EXPECT_FALSE( HasKey( layout, "Interactable" ) );
    EXPECT_FALSE( HasKey( layout, "RaycastTarget" ) );
}

// ── Missing and broken fields ──────────────────────────────────────────────────────────────────────

TEST( SceneUIVisibilityMigration, APayloadWithNeitherKeyIsLeftByteIdentical )
{
    std::vector<Assets::EntityData> entities{ ElementWith( Keys{ { "Pivot", rfl::Generic( 0.5 ) } } ) };
    const std::string               before = rfl::json::write( entities[0] );

    const auto report = Migration::MigrateUIVisibilityV9ToV10( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.FlagsDropped, 0 );
    EXPECT_EQ( rfl::json::write( entities[0] ), before );
}

// An entity with no UILayout at all — every non-UI entity in every scene — is not touched and does not
// count. Without this the sweep at the bottom would be measuring nothing on 65 of the 67 scenes.
TEST( SceneUIVisibilityMigration, AnEntityWithNoUILayoutIsNotTouched )
{
    Assets::EntityData mesh;
    mesh.Tag = "Ground";
    mesh.Components["StaticMesh"] =
         rfl::Generic( ObjectOf( Keys{ { "Primitive", rfl::Generic( std::string( "Cube" ) ) } } ) );
    std::vector<Assets::EntityData> entities{ mesh };
    const std::string               before = rfl::json::write( entities[0] );

    const auto report = Migration::MigrateUIVisibilityV9ToV10( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( rfl::json::write( entities[0] ), before );
}

// A VALUE THAT IS NOT A BOOLEAN IS REFUSED AND NAMED, not guessed at. There is no behaviour to carry from
// the string "yes" or from 1, and picking one would be the silent substitution DC 1.4 forbids. The key
// still goes — the field it names does not exist any more — and the element keeps the default.
TEST( SceneUIVisibilityMigration, ANonBooleanFlagIsDroppedNamedAndLeavesTheDefault )
{
    std::vector<Assets::EntityData> entities{
         ElementWith( Keys{ { "Interactable", rfl::Generic( std::string( "yes" ) ) } }, "Confirm Button" ),
         ElementWith( Keys{ { "RaycastTarget", rfl::Generic( 0 ) } }, "Scrim" ) };

    const auto report = Migration::MigrateUIVisibilityV9ToV10( entities );

    EXPECT_EQ( report.Entities, 2 );
    EXPECT_EQ( report.FlagsDropped, 2 );
    EXPECT_EQ( report.HitTestSet, 0 ) << "a value nobody can read must not decide the element's hit testing";

    ASSERT_EQ( report.BrokenNames.size(), 2u );
    EXPECT_EQ( report.BrokenNames[0], "Confirm Button.Interactable" );
    EXPECT_EQ( report.BrokenNames[1], "Scrim.RaycastTarget" )
         << "the operator has to be told WHICH element, not how many";

    for ( const auto& entity : entities )
    {
        EXPECT_FALSE( HasKey( LayoutOf( entity ), "HitTest" ) );
        EXPECT_FALSE( HasKey( LayoutOf( entity ), "Interactable" ) );
        EXPECT_FALSE( HasKey( LayoutOf( entity ), "RaycastTarget" ) );
    }
}

// A UILayout that is not an object at all — a hand-edit that put a number or a string there. The step says
// so and moves on rather than replacing the payload with one of its own making.
TEST( SceneUIVisibilityMigration, AUILayoutThatIsNotAnObjectIsLeftAlone )
{
    Assets::EntityData broken;
    broken.Tag                    = "Bad";
    broken.Components["UILayout"] = rfl::Generic( 7 );
    std::vector<Assets::EntityData> entities{ broken };
    const std::string               before = rfl::json::write( entities[0] );

    const auto report = Migration::MigrateUIVisibilityV9ToV10( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( rfl::json::write( entities[0] ), before );
}

// ── What it must NOT do ────────────────────────────────────────────────────────────────────────────

// THE OTHER AXIS IS NOT INVENTED. `Visibility` is new and the v9 format had no way to say anything but
// "visible" about an element — the one authored `Visible` bool in the whole UI set lived on the CANVAS and
// this step does not touch it. Writing Visible into every element would be a value nobody authored.
TEST( SceneUIVisibilityMigration, VisibilityIsNeverWritten )
{
    std::vector<Assets::EntityData> entities{ ElementWith( Keys{ { "Interactable", rfl::Generic( false ) } } ),
                                              ElementWith( Keys{ { "RaycastTarget", rfl::Generic( false ) } } ),
                                              ElementWith( Keys{ { "Pivot", rfl::Generic( 0.5 ) } } ) };

    Migration::MigrateUIVisibilityV9ToV10( entities );

    for ( const auto& entity : entities )
        EXPECT_FALSE( HasKey( LayoutOf( entity ), "Visibility" ) )
             << "the old format could not state this, so any value written here is invented";
}

// Everything else in the payload survives, in order. A migration that reorders the file makes every future
// diff unreadable, and one that drops a neighbouring key is a data loss nobody would notice for months.
TEST( SceneUIVisibilityMigration, TheRestOfThePayloadSurvivesInOrder )
{
    std::vector<Assets::EntityData> entities{ ElementWith( Keys{ { "RaycastTarget", rfl::Generic( false ) } } ) };

    Migration::MigrateUIVisibilityV9ToV10( entities );

    const std::vector<std::string> keys = KeysOf( LayoutOf( entities[0] ) );
    ASSERT_EQ( keys.size(), 3u ) << "AnchorMin, ClipContents and the new HitTest, and nothing else";
    EXPECT_EQ( keys[0], "AnchorMin" );
    EXPECT_EQ( keys[1], "ClipContents" );
    EXPECT_EQ( keys[2], "HitTest" );
}

// Idempotent: the second run has nothing to find, so the tree comes out byte-identical and reports zero.
TEST( SceneUIVisibilityMigration, RunningItTwiceChangesNothingTheSecondTime )
{
    std::vector<Assets::EntityData> entities{ ElementWith( Keys{ { "Interactable", rfl::Generic( false ) } } ) };

    Migration::MigrateUIVisibilityV9ToV10( entities );
    const std::string afterFirst = rfl::json::write( entities[0] );
    const auto        second     = Migration::MigrateUIVisibilityV9ToV10( entities );

    EXPECT_EQ( second.Entities, 0 );
    EXPECT_EQ( second.FlagsDropped, 0 );
    EXPECT_EQ( rfl::json::write( entities[0] ), afterFirst );
}

// ── Through the one entry point, and gated on its own integer ──────────────────────────────────────

TEST( SceneUIVisibilityMigration, MigrateSceneRunsItForAV9FileAndStampsTheHead )
{
    Desert::Migration::SceneSerialized scene;
    scene.SceneName    = "Fixture";
    scene.SceneVersion = 9;
    scene.UnitVersion  = Core::kUnitVersion;
    scene.Entities.push_back( ElementWith( Keys{ { "RaycastTarget", rfl::Generic( false ) } } ) );

    const auto report = Migration::MigrateScene( scene, "Resources/Assets" );

    EXPECT_TRUE( report.UIVisibilityRaised );
    EXPECT_EQ( report.UIVisibility.HitTestSet, 1 );
    ASSERT_TRUE( scene.Header.has_value() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Core::kSceneVersion );
    EXPECT_EQ( *IntAt( LayoutOf( scene.Entities[0] ), "HitTest" ),
               static_cast<int>( ECS::UIHitTest::ChildrenOnly ) );
}

// A file already at the head does not run the step again. Gating on the step's OWN integer rather than on
// kSceneVersion is what stops the next head bump from re-running every earlier step.
TEST( SceneUIVisibilityMigration, AFileAtTheHeadDoesNotRunTheStep )
{
    Desert::Migration::SceneSerialized scene;
    scene.SceneVersion = Core::kSceneVersion;
    scene.UnitVersion  = Core::kUnitVersion;

    EXPECT_FALSE( Migration::MigrateScene( scene, "Resources/Assets" ).UIVisibilityRaised );
}

TEST( SceneUIVisibilityMigration, TheStepHasItsOwnVersionAndItIsBelowTheHead )
{
    EXPECT_EQ( 10, Migration::kSceneVersionUIVisibility );
    // It was the newest step when it was written and is not any more (v11 restates SSRMaxDistance in
    // centimetres). What has to stay true is that it is BELOW the head — a step at or above the
    // generation the loader requires would stamp files the loader refuses.
    EXPECT_LT( Migration::kSceneVersionUIVisibility, Core::kSceneVersion );
    EXPECT_GT( Migration::kSceneVersionUIVisibility, Migration::kSceneVersionGravityUnits );
}

// ── THE CORPUS: the conversion actually happened in the FILES ──────────────────────────────────────
//
// DEV_CONTRACT §4.5 — the scenes in the repository are converted by the same task. A migration that is
// only correct in a unit test leaves 67 files the loader refuses, and the refusal is the only symptom.
TEST( SceneUIVisibilityMigrationCorpus, NoShippedSceneStillStatesTheOldBooleans )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the test's working directory";

    const std::filesystem::path scenes = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( scenes ) ) << scenes.string();

    int layouts = 0;
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

        for ( const auto& entity : parsed.value().Entities )
        {
            const auto layout = entity.Components.get( "UILayout" );
            if ( !layout.has_value() )
                continue;
            const auto fields = layout.value().to_object();
            if ( !fields.has_value() )
                continue;

            ++layouts;
            EXPECT_FALSE( HasKey( fields.value(), "Interactable" ) )
                 << entry.path().string() << " still states UILayout.Interactable — run Tools/SceneMigrator";
            EXPECT_FALSE( HasKey( fields.value(), "RaycastTarget" ) )
                 << entry.path().string() << " still states UILayout.RaycastTarget — run Tools/SceneMigrator";
        }
    }

    EXPECT_GT( layouts, 0 ) << "no scene carries a UI element at all - this test would pass vacuously";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
