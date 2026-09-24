// THE PREFAB MIGRATION, held to the relation И11 exists to create: A PREFAB AND A SCENE ARE RAISED BY
// THE SAME CHAIN.
//
// Prefabs have always shared the scene's two version integers — a `.deprefab` carries the scene's own
// EntityData, written by the same ComponentRegistry — but their migrator knew exactly ONE step: stamp an
// unversioned file at the head. So the moment Core::kSceneVersion moved, every existing prefab was left
// at the old number with no route forward: the loader refused it, and so did the thing the refusal named.
// The version was shared and the machinery was not.
//
// The fix is not a second numbering scheme; it is one chain, in SceneMigration.cpp, entered by both
// MigrateScene and MigratePrefab. This suite pins that:
//
//   1. THE RELATION: for the same entities at the same stated version, the prefab path and the scene path
//      produce BYTE-IDENTICAL entities. This is the test that fails if a future entity-level step is
//      wired into one entry point and not the other — which is the whole defect class, one indirection
//      out.
//   2. an older STAMPED prefab is actually raised, step by step, not merely re-stamped;
//   3. the settings-only steps do not run and nothing is fabricated for a file that has no Settings block;
//   4. the unstamped (v0/v0) file is still STAMP-ONLY, deliberately, and says so;
//   5. a generation this tool cannot place — later than the head, or half-stamped — is refused by its own
//      numbers and the tree is not touched;
//   6. what the migration stamps is what the ENGINE'S OWN gate requires.
//
// PURE, like the migration: no filesystem, no GPU.

#include <SceneMigration.hpp>

#include <Engine/Assets/Prefab/PrefabFormat.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Assets::PrefabData;
using Desert::Assets::PrefabIsAtCurrentVersion;
using Desert::Core::kSceneVersion;
using Desert::Core::kUnitVersion;
using Desert::Core::SceneSerialized;
using Desert::Migration::MigratePrefab;
using Desert::Migration::MigrateScene;

namespace
{
    // One entity carrying a "Script" payload in the pre-v16 shape: the slot names its `.lua` by the path
    // the editor was standing in. The v15 -> v16 step renames the key WITH the value, so a raised tree is
    // recognizable by inspection rather than by trusting a counter.
    EntityData Scripted()
    {
        rfl::Generic::Object slot;
        slot["Path"]  = rfl::Generic( std::string( "Resources/Assets/Scripts/Examples/MoveAlongX.lua" ) );
        slot["Props"] = rfl::Generic( rfl::Generic::Array{} );

        rfl::Generic::Object payload;
        payload["Scripts"] = rfl::Generic( rfl::Generic::Array{ rfl::Generic( slot ) } );

        EntityData entity;
        entity.id                   = Common::UUID( 1001ull );
        entity.Tag                  = "Player";
        entity.Components["Script"] = rfl::Generic( payload );
        return entity;
    }

    // A second entity with a UI payload in the pre-v10 shape, so more than one entity-level step has
    // something to find and "the chain ran" is not one step's word.
    EntityData UIElement()
    {
        rfl::Generic::Object layout;
        layout["Interactable"]  = rfl::Generic( true );
        layout["RaycastTarget"] = rfl::Generic( false );

        EntityData entity;
        entity.id                     = Common::UUID( 1002ull );
        entity.parent                 = Common::UUID( 1001ull );
        entity.Tag                    = "Button";
        entity.Components["UILayout"] = rfl::Generic( layout );
        return entity;
    }

    std::vector<EntityData> Payload()
    {
        return { Scripted(), UIElement() };
    }

    // The name is shared with the scene fixture on purpose: the v11 -> v12 step derives a material name
    // from it, so two trees can only be compared byte for byte if they agree about it.
    constexpr const char* kName = "Fixture";

    PrefabData PrefabAt( int sceneVersion, int unitVersion )
    {
        PrefabData prefab;
        prefab.Name     = kName;
        prefab.Entities = Payload();
        prefab.Root     = Common::UUID( 1001ull );
        if ( sceneVersion != 0 )
            prefab.SceneVersion = sceneVersion;
        if ( unitVersion != 0 )
            prefab.UnitVersion = unitVersion;
        return prefab;
    }

    SceneSerialized SceneAt( int sceneVersion, int unitVersion )
    {
        SceneSerialized scene;
        scene.SceneName    = kName;
        scene.Entities     = Payload();
        scene.SceneVersion = sceneVersion;
        scene.UnitVersion  = unitVersion;
        return scene;
    }

    std::string EntitiesJson( const std::vector<EntityData>& entities )
    {
        return rfl::json::write( entities );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. THE RELATION: one chain, entered from two doors
// ---------------------------------------------------------------------------------------------------

// If somebody adds an entity-level step to MigrateScene and forgets MigratePrefab, this is the assertion
// that says so — before a version bump turns every prefab in a project into a file the loader refuses.
// It is stated over the whole entity tree rather than over a list of step flags precisely so that a step
// which does not exist yet is covered by it.
TEST( PrefabMigration, TheSameEntitiesComeOutOfBothEntryPointsByteForByte )
{
    // Every stamped generation this tool has a route from, not just the newest: a prefab entering the
    // chain at v3 must arrive by the same road a scene entering at v3 does.
    for ( int from = 1; from < kSceneVersion; ++from )
    {
        PrefabData      prefab = PrefabAt( from, kUnitVersion );
        SceneSerialized scene  = SceneAt( from, kUnitVersion );

        const auto outcome = MigratePrefab( prefab );
        const auto report  = MigrateScene( scene );

        ASSERT_TRUE( outcome.Refused.empty() ) << "v" << from << ": " << outcome.Refused;
        ASSERT_TRUE( report.Refused.empty() ) << "v" << from << ": " << report.Refused;

        // The one step a scene takes and a prefab does not (v25, MigrateSiblingOrderV24ToV25): a scene
        // states each record's sibling index and sorts by id, a prefab keeps hierarchy order. Everything
        // else must match byte for byte, so only that field is set aside.
        for ( auto& record : scene.Entities )
            record.siblingIndex.reset();
        EXPECT_EQ( EntitiesJson( prefab.Entities ), EntitiesJson( scene.Entities ) )
             << "entering at v" << from
             << ", the prefab entry point and the scene entry point disagree about the entities - the two "
                "are not running the same chain any more";
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. An older STAMPED prefab is RAISED, not re-stamped
// ---------------------------------------------------------------------------------------------------

TEST( PrefabMigration, AStampedOlderPrefabRunsTheEntityStepsAndArrivesAtTheHead )
{
    PrefabData prefab = PrefabAt( 1, kUnitVersion );

    const auto outcome = MigratePrefab( prefab );

    ASSERT_TRUE( outcome.Refused.empty() ) << outcome.Refused;
    EXPECT_FALSE( outcome.AlreadyCurrent );
    EXPECT_FALSE( outcome.StampOnly ) << "a stamped file has a generation to migrate FROM; stamping it "
                                         "would be the silent no-op И11 exists to remove";
    EXPECT_EQ( outcome.FoundSceneVersion, 1 );

    // The v15 -> v16 step ran: the key is renamed WITH its value, so the old spelling is gone.
    ASSERT_EQ( prefab.Entities.size(), 2u );
    const std::string json = EntitiesJson( prefab.Entities );
    EXPECT_NE( json.find( "ScriptKey" ), std::string::npos ) << json;
    EXPECT_EQ( json.find( "\"Path\"" ), std::string::npos ) << json;
    EXPECT_EQ( outcome.Steps.ScriptRoot.Slots, 1 );

    // And so did the v9 -> v10 one, on a different entity and a different payload.
    EXPECT_EQ( outcome.Steps.UIVisibility.Entities, 1 );
    EXPECT_EQ( json.find( "Interactable" ), std::string::npos ) << json;

    ASSERT_TRUE( prefab.SceneVersion.has_value() );
    ASSERT_TRUE( prefab.UnitVersion.has_value() );
    EXPECT_EQ( *prefab.SceneVersion, kSceneVersion );
    EXPECT_EQ( *prefab.UnitVersion, kUnitVersion );
}

// ---------------------------------------------------------------------------------------------------
// 3. The settings-only steps do not run, and nothing is fabricated
// ---------------------------------------------------------------------------------------------------

// A `.deprefab` has no scene-wide Settings block. The four steps that touch only that block are gated on
// the block EXISTING rather than on a per-file-class flag, and this pins the consequence: they report
// nothing, and in particular the tonemap step — the one that CREATES a settings block when it finds none
// — does not run and so has no created block to be quietly dropped on the way out.
TEST( PrefabMigration, TheSettingsOnlyStepsAreSkippedForAFileThatHasNoSettingsBlock )
{
    PrefabData prefab = PrefabAt( 1, kUnitVersion );

    const auto outcome = MigratePrefab( prefab );

    ASSERT_TRUE( outcome.Refused.empty() ) << outcome.Refused;
    EXPECT_FALSE( outcome.Steps.TonemapperRaised );
    EXPECT_FALSE( outcome.Steps.GravityUnitsRaised );
    EXPECT_FALSE( outcome.Steps.SSRUnitsRaised );
    EXPECT_FALSE( outcome.Steps.DebugViewRaised );

    // The entity-level ones did run, so the four above are silent because the block is absent and not
    // because the chain was never entered.
    EXPECT_TRUE( outcome.Steps.ScriptRootRaised );
    EXPECT_TRUE( outcome.Steps.UIVisibilityRaised );
}

// ---------------------------------------------------------------------------------------------------
// 4. The unstamped file: STAMP ONLY, on purpose
// ---------------------------------------------------------------------------------------------------

// A `.desce` at v0 provably predates v1 — scenes were stamped from the beginning. A `.deprefab` at v0
// could be from ANY generation up to the one Д28 landed in, so running the chain on it would be guessing
// which, and the sky and unit steps are the two that cannot be re-run safely. The file is stamped, the
// entities are left exactly as found, and the outcome SAYS it was a stamp so the tool can say so too.
TEST( PrefabMigration, AnUnversionedPrefabIsStampedAtBothHeadsAndItsEntitiesDoNotMove )
{
    PrefabData        prefab = PrefabAt( 0, 0 );
    const std::string before = EntitiesJson( prefab.Entities );

    const auto outcome = MigratePrefab( prefab );

    EXPECT_TRUE( outcome.StampOnly );
    EXPECT_FALSE( outcome.AlreadyCurrent );
    EXPECT_TRUE( outcome.Refused.empty() ) << outcome.Refused;
    EXPECT_EQ( outcome.FoundSceneVersion, 0 );
    EXPECT_EQ( outcome.FoundUnitVersion, 0 );

    ASSERT_TRUE( prefab.SceneVersion.has_value() );
    ASSERT_TRUE( prefab.UnitVersion.has_value() );
    EXPECT_EQ( *prefab.SceneVersion, kSceneVersion );
    EXPECT_EQ( *prefab.UnitVersion, kUnitVersion );

    // Byte-for-byte: "stamp only" is measured, not asserted by the code path that claims it.
    EXPECT_EQ( EntitiesJson( prefab.Entities ), before );
    EXPECT_EQ( prefab.Name, kName );
}

// ---------------------------------------------------------------------------------------------------
// 5. What is not migrated: the current file, and the generations that cannot be placed
// ---------------------------------------------------------------------------------------------------

TEST( PrefabMigration, ACurrentPrefabIsReportedCurrentAndLeftAlone )
{
    PrefabData        prefab = PrefabAt( kSceneVersion, kUnitVersion );
    const std::string before = rfl::json::write( prefab );

    const auto outcome = MigratePrefab( prefab );

    EXPECT_TRUE( outcome.AlreadyCurrent );
    EXPECT_FALSE( outcome.StampOnly );
    EXPECT_TRUE( outcome.Refused.empty() ) << outcome.Refused;
    EXPECT_EQ( rfl::json::write( prefab ), before );
}

// A file stamped LATER than the head was written by a build this tool does not know, and one stamped on
// only one axis was hand-edited: the one writer of prefab text stamps both together. Stamping over either
// is the silent substitution the gate forbids — the numbers are named and the tree is untouched.
TEST( PrefabMigration, AGenerationThisToolCannotPlaceIsRefusedByItsOwnNumbersAndNotTouched )
{
    struct Case
    {
        int Scene;
        int Unit;
    };
    const Case cases[] = { { kSceneVersion + 1, kUnitVersion }, // later on the schema axis
                           { kSceneVersion, kUnitVersion + 1 }, // later on the unit axis
                           { kSceneVersion, 0 },                // stamped on one axis only
                           { 0, kUnitVersion } };               // the other half of the same shape

    for ( const Case& c : cases )
    {
        PrefabData        prefab = PrefabAt( c.Scene, c.Unit );
        const std::string before = rfl::json::write( prefab );

        const auto outcome = MigratePrefab( prefab );

        EXPECT_FALSE( outcome.AlreadyCurrent ) << "v" << c.Scene << "/v" << c.Unit;
        EXPECT_FALSE( outcome.StampOnly ) << "v" << c.Scene << "/v" << c.Unit;
        ASSERT_FALSE( outcome.Refused.empty() ) << "v" << c.Scene << "/v" << c.Unit << " was not refused";
        EXPECT_NE( outcome.Refused.find( "v" + std::to_string( c.Scene ) ), std::string::npos ) << outcome.Refused;
        EXPECT_NE( outcome.Refused.find( "v" + std::to_string( c.Unit ) ), std::string::npos ) << outcome.Refused;
        EXPECT_EQ( rfl::json::write( prefab ), before ) << "a refused tree was modified";
    }
}

// The same guard on the scene door, and it is new: every gate in the chain is `stated < step`, so a tree
// from a LATER build matched no step and fell through to the unconditional stamp — the tool wrote its own
// head over a higher number while the payloads stayed where they were, then reported the file as already
// current. Refused now, from the same place, in the same words.
TEST( PrefabMigration, ASceneFromALaterBuildIsRefusedRatherThanStampedDown )
{
    SceneSerialized scene = SceneAt( kSceneVersion + 1, kUnitVersion );

    const auto report = MigrateScene( scene );

    ASSERT_FALSE( report.Refused.empty() ) << "a later-generation scene was not refused";
    EXPECT_FALSE( report.Changed() );
    ASSERT_TRUE( scene.SceneVersion.has_value() );
    EXPECT_EQ( *scene.SceneVersion, kSceneVersion + 1 ) << "the stated version was overwritten downwards";
}

// ---------------------------------------------------------------------------------------------------
// 6. THE OTHER RELATION: the version the migration writes is the version the engine requires
// ---------------------------------------------------------------------------------------------------

// Asserted through the LOADER'S OWN gate function, not by comparing two spelled-out numbers: if the
// migration ever stamps anything Core::kSceneVersion is not, every migrated prefab would be refused by
// the engine at once, and this is the test that says so before a user does.
TEST( PrefabMigration, WhatTheMigrationStampsTheEngineGateAccepts )
{
    for ( int from = 0; from < kSceneVersion; ++from )
    {
        PrefabData prefab = PrefabAt( from, from == 0 ? 0 : kUnitVersion );
        ASSERT_FALSE( PrefabIsAtCurrentVersion( prefab ) ) << "the fixture must start below the gate";

        const auto outcome = MigratePrefab( prefab );
        ASSERT_TRUE( outcome.Refused.empty() ) << "v" << from << ": " << outcome.Refused;

        EXPECT_TRUE( PrefabIsAtCurrentVersion( prefab ) )
             << "entering at v" << from
             << ", MigratePrefab produced a tree the engine's own gate refuses - the migration and the "
                "loader disagree about what the current generation is";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
