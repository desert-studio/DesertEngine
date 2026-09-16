// The v20 -> v21 migration: the anim graph stops being a blob inside the entity and becomes a file.
//
// §5.1 named two graphs with OPPOSITE identity defects. The shader graph had a file and no asset type;
// this one had the right window — a document with a subject and a liveness answer — and the wrong
// storage: `AnimationComponentSer::GraphJson` carried the whole state machine inside every entity that
// used one. Two characters could not share a walk graph, and copying a character copied a blob that then
// drifted from its original with nothing able to notice.
//
// WHAT IS ASSERTED:
//
//   1. A non-empty blob becomes a FILE plus a relative path under "Graph", and the blob key is gone.
//   2. The file is named after the GRAPH, so two entities carrying identical blobs converge on ONE path
//      — which is the whole point of the step and the thing the old storage made impossible.
//   3. Two entities whose graphs differ produce two files.
//   4. An EMPTY blob drops the key and produces no file: an entity with an Animation component and no
//      state machine is the common case by count.
//   5. A blob that will not parse is LEFT IN PLACE and named. Dropping it would lose an artist's work to
//      make a counter go up, and writing it unread would produce a file the engine then refuses.
//   6. The shapes a hand-edited file has: a payload that is not an object, a field that is not a string.
//   7. It is IDEMPOTENT — a converted payload has no GraphJson key, so a second pass changes nothing.
//   8. A graph Name that is not a filename cannot escape the AnimGraphs/ folder.
//   9. The step runs through the version gate, exactly once, on files below its own number.
//  10. The step is the head, and the head is what the engine requires.

#include <SceneMigration.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Desert::Migration::AnimGraphMigrationReport;
using Desert::Migration::MigrateAnimGraphV20ToV21;

namespace
{
    // One entity carrying an "Animation" payload with the given GraphJson value, verbatim.
    Desert::Assets::EntityData EntityWithBlob( const std::string& tag, const rfl::Generic& graphJson )
    {
        Desert::Assets::EntityData entity;
        entity.Tag = tag;

        rfl::Generic::Object animation;
        animation["CurrentClip"]   = rfl::Generic( std::string{} );
        animation["Playing"]       = rfl::Generic( true );
        animation["Loop"]          = rfl::Generic( true );
        animation["PlaybackSpeed"] = rfl::Generic( 1.0 );
        animation["GraphJson"]     = graphJson;

        entity.Components["Animation"] = rfl::Generic( std::move( animation ) );
        return entity;
    }

    std::string BlobFor( const std::string& name, const std::string& clip )
    {
        Desert::Animation::Graph::AnimGraph graph;
        graph.Name = name;
        Desert::Animation::Graph::State state;
        state.Name = "Idle";
        state.Clip = clip;
        graph.States.push_back( state );
        graph.Entry = "Idle";
        return Desert::Animation::Graph::Serialize( graph );
    }

    const rfl::Generic::Object& AnimationOf( const Desert::Assets::EntityData& entity )
    {
        static rfl::Generic::Object empty;
        const auto                  payload = entity.Components.get( "Animation" );
        if ( !payload.has_value() )
            return empty;
        const auto object = payload.value().to_object();
        if ( !object.has_value() )
            return empty;
        static rfl::Generic::Object held;
        held = object.value();
        return held;
    }
} // namespace

TEST( SceneAnimGraphMigration, ABlobBecomesAFileAndTheEntityNamesIt )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "Hero", rfl::Generic( BlobFor( "Locomotion", "Walk" ) ) ) );

    const AnimGraphMigrationReport report = MigrateAnimGraphV20ToV21( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Rejected, 0 );
    ASSERT_EQ( report.Graphs.size(), 1u );
    EXPECT_EQ( report.Graphs[0].RelativePath, "AnimGraphs/Locomotion.danimgraph" );

    // The bytes are the graph, readable by the engine's own parser and not by a second one written here.
    const auto reparsed = Desert::Animation::Graph::Deserialize( report.Graphs[0].Json );
    ASSERT_TRUE( reparsed.IsSuccess() ) << reparsed.GetError();
    EXPECT_EQ( reparsed.GetValue().Name, "Locomotion" );
    ASSERT_EQ( reparsed.GetValue().States.size(), 1u );
    EXPECT_EQ( reparsed.GetValue().States[0].Clip, "Walk" );

    const auto& animation = AnimationOf( entities[0] );
    EXPECT_FALSE( animation.get( "GraphJson" ).has_value() ) << "the blob key survived the step";
    ASSERT_TRUE( animation.get( "Graph" ).has_value() );
    EXPECT_EQ( animation.get( "Graph" )->to_string().value(), "AnimGraphs/Locomotion.danimgraph" );

    // Everything else in the payload is untouched — a step that rewrote the playback settings while
    // moving the graph would be doing two things and only saying one.
    EXPECT_TRUE( animation.get( "Playing" )->to_bool().value() );
    EXPECT_FLOAT_EQ( static_cast<float>( animation.get( "PlaybackSpeed" )->to_double().value() ), 1.0f );
}

TEST( SceneAnimGraphMigration, TwoEntitiesWithTheSameGraphConvergeOnOneFile )
{
    // THE POINT OF THE WHOLE STEP. This is what copying a character produced under the old storage: two
    // byte-identical blobs that were then two separate graphs forever. Measured on this repository's own
    // corpus, four entities across two scenes converged on AnimGraphs/ScriptDriven.danimgraph.
    const std::string blob = BlobFor( "ScriptDriven", "Wave" );

    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "A", rfl::Generic( blob ) ) );
    entities.push_back( EntityWithBlob( "B", rfl::Generic( blob ) ) );

    const AnimGraphMigrationReport report = MigrateAnimGraphV20ToV21( entities );

    EXPECT_EQ( report.Entities, 2 );
    ASSERT_EQ( report.Graphs.size(), 2u ) << "the step is per entity; the WRITER collapses equal bytes";
    EXPECT_EQ( report.Graphs[0].RelativePath, report.Graphs[1].RelativePath );
    EXPECT_EQ( report.Graphs[0].Json, report.Graphs[1].Json )
         << "two entities landed on one path with DIFFERENT bytes — the tool would refuse the run, and "
            "rightly, because one of the two graphs would otherwise be silently replaced by the other";
}

TEST( SceneAnimGraphMigration, TwoDifferentGraphsGetTwoFiles )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "A", rfl::Generic( BlobFor( "Walk", "Walk" ) ) ) );
    entities.push_back( EntityWithBlob( "B", rfl::Generic( BlobFor( "Run", "Run" ) ) ) );

    const AnimGraphMigrationReport report = MigrateAnimGraphV20ToV21( entities );

    ASSERT_EQ( report.Graphs.size(), 2u );
    EXPECT_NE( report.Graphs[0].RelativePath, report.Graphs[1].RelativePath );
}

TEST( SceneAnimGraphMigration, AnEmptyBlobDropsTheKeyAndWritesNoFile )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "Crate", rfl::Generic( std::string{} ) ) );

    const AnimGraphMigrationReport report = MigrateAnimGraphV20ToV21( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Empty, 1 );
    EXPECT_TRUE( report.Graphs.empty() ) << "an entity with no state machine produced a file";

    const auto& animation = AnimationOf( entities[0] );
    EXPECT_FALSE( animation.get( "GraphJson" ).has_value() );
    EXPECT_FALSE( animation.get( "Graph" ).has_value() )
         << "absence is how this format says 'no state machine'; an empty string would be a second "
            "spelling of it that every reader then has to agree about";
}

TEST( SceneAnimGraphMigration, AnUnparseableBlobIsLeftInPlaceAndNamed )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "Broken", rfl::Generic( std::string( "{ not json" ) ) ) );

    const AnimGraphMigrationReport report = MigrateAnimGraphV20ToV21( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Rejected, 1 );
    EXPECT_TRUE( report.Graphs.empty() );
    ASSERT_EQ( report.RejectedNames.size(), 1u );
    EXPECT_NE( report.RejectedNames[0].find( "Broken" ), std::string::npos )
         << "the refusal does not name the entity, so an operator cannot find it: "
         << report.RejectedNames[0];

    // LEFT IN PLACE. The work is still in the file and a fixed tool can have another go; dropping it
    // would lose an authored state machine to make a counter go up.
    EXPECT_TRUE( AnimationOf( entities[0] ).get( "GraphJson" ).has_value() );
}

TEST( SceneAnimGraphMigration, AFieldOfTheWrongTypeIsRefusedRatherThanRead )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "Odd", rfl::Generic( 7.0 ) ) );

    const AnimGraphMigrationReport report = MigrateAnimGraphV20ToV21( entities );

    EXPECT_EQ( report.Rejected, 1 );
    EXPECT_TRUE( report.Graphs.empty() );
    EXPECT_TRUE( AnimationOf( entities[0] ).get( "GraphJson" ).has_value() );
}

TEST( SceneAnimGraphMigration, APayloadThatIsNotAnObjectIsLeftAlone )
{
    Desert::Assets::EntityData entity;
    entity.Tag                     = "Hand-edited";
    entity.Components["Animation"] = rfl::Generic( std::string( "this is not a payload" ) );

    std::vector<Desert::Assets::EntityData> entities{ entity };
    const AnimGraphMigrationReport          report = MigrateAnimGraphV20ToV21( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Rejected, 0 ) << "a malformed payload is not a rejected GRAPH — it has none";
    EXPECT_TRUE( report.Graphs.empty() );
}

TEST( SceneAnimGraphMigration, ASecondPassChangesNothing )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "Hero", rfl::Generic( BlobFor( "Locomotion", "Walk" ) ) ) );

    const AnimGraphMigrationReport first = MigrateAnimGraphV20ToV21( entities );
    ASSERT_EQ( first.Graphs.size(), 1u );
    const std::string afterFirst = rfl::json::write( entities[0].Components );

    const AnimGraphMigrationReport second = MigrateAnimGraphV20ToV21( entities );

    EXPECT_EQ( second.Entities, 0 );
    EXPECT_TRUE( second.Graphs.empty() )
         << "a second pass produced a file again — the converted payload still carries a GraphJson key, "
            "or the step decides from something other than that key's presence";
    EXPECT_EQ( rfl::json::write( entities[0].Components ), afterFirst );
}

TEST( SceneAnimGraphMigration, AGraphNameCannotEscapeTheFolder )
{
    // An artist types anything into the Name field. Without the sanitisation this is the one way a
    // step that looks pure reaches a file nobody asked it to touch.
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "Sneaky", rfl::Generic( BlobFor( "../../etc/passwd", "Walk" ) ) ) );

    const AnimGraphMigrationReport report = MigrateAnimGraphV20ToV21( entities );

    ASSERT_EQ( report.Graphs.size(), 1u );
    EXPECT_EQ( report.Graphs[0].RelativePath.find( ".." ), std::string::npos )
         << "the written path still climbs out of the assets tree: " << report.Graphs[0].RelativePath;
    EXPECT_EQ( report.Graphs[0].RelativePath.rfind( "AnimGraphs/", 0 ), 0u );
}

TEST( SceneAnimGraphMigration, AGraphWithNoNameIsNamedAfterItsEntity )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWithBlob( "Villager", rfl::Generic( BlobFor( "", "Walk" ) ) ) );

    const AnimGraphMigrationReport report = MigrateAnimGraphV20ToV21( entities );

    ASSERT_EQ( report.Graphs.size(), 1u );
    EXPECT_EQ( report.Graphs[0].RelativePath, "AnimGraphs/Villager.danimgraph" )
         << "an unnamed graph took a constant name, which would collide every such graph in the project "
            "into one file and make four characters share a state machine none of them authored";
}

TEST( SceneAnimGraphMigration, TheStepRunsThroughTheGateExactlyOnceAndOnlyBelowItsNumber )
{
    const auto sceneWith = []( int version, const std::string& blob )
    {
        Desert::Core::SceneSerialized scene;
        scene.SceneVersion = version;
        scene.UnitVersion  = Desert::Core::kUnitVersion;
        scene.Entities.push_back( EntityWithBlob( "Hero", rfl::Generic( blob ) ) );
        return scene;
    };

    // Below the step: it runs, and the file comes back.
    auto       below  = sceneWith( Desert::Migration::kSceneVersionAnimGraphAsset - 1,
                                   BlobFor( "Locomotion", "Walk" ) );
    const auto raised = Desert::Migration::MigrateScene( below );
    EXPECT_TRUE( raised.AnimGraphRaised );
    ASSERT_EQ( raised.AnimGraph.Graphs.size(), 1u );
    EXPECT_EQ( below.SceneVersion.value_or( 0 ), Desert::Core::kSceneVersion );

    // AT the step: a file already at the head is not run through it again.
    auto       current = sceneWith( Desert::Core::kSceneVersion, BlobFor( "Locomotion", "Walk" ) );
    const auto again   = Desert::Migration::MigrateScene( current );
    EXPECT_FALSE( again.AnimGraphRaised )
         << "a file already at the head was sent through the step, which is how a step that decides FROM "
            "a value comes to run twice";
    EXPECT_TRUE( again.AnimGraph.Graphs.empty() );
}

TEST( SceneAnimGraphMigration, TheStepIsTheHeadAndTheHeadIsWhatTheEngineRequires )
{
    // The same relation SceneMigration.hpp asserts at compile time, restated at run time so a reader of
    // this suite can see which number the step is without opening the header.
    EXPECT_EQ( Desert::Migration::kSceneVersionAnimGraphAsset, Desert::Core::kSceneVersion );
    EXPECT_EQ( Desert::Migration::kSceneVersionAnimGraphAsset,
               Desert::Migration::kSceneVersionTextKeySigil + 1 )
         << "step 21 is no longer the one after 20 — two steps share a number, which is the collision "
            "that cost this project a merge (see kSceneVersionTextKeySigil's own note)";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
