// `.danimgraph` as an ASSET: what the file has to do, and the ONE relation the whole of §5.1's animation
// half rests on — a graph shared by several entities is ONE OBJECT, not a copy per entity.
//
// The old storage was `AnimationComponentSer::GraphJson`, the whole state machine inside every entity that
// used one. Two characters could not share a walk graph; copying a character copied a blob that then
// drifted from its original with nothing able to see it. The cure is an asset and a handle, and the cure
// only works if the OBJECT is shared as well as the file — a per-entity copy would put the edit back
// inside one entity and reproduce the defect with extra steps.

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/AnimGraphAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using Desert::Animation::Graph::AnimGraph;
using Desert::Animation::Graph::Evaluator;
using Desert::Animation::Graph::State;
using Desert::Assets::AnimGraphAsset;
using Desert::Assets::AssetPriority;

namespace
{
    AnimGraph Locomotion()
    {
        AnimGraph g;
        g.Name = "Locomotion";
        State idle;
        idle.Name = "Idle";
        idle.Clip = "Anim_Idle";
        g.States.push_back( idle );
        g.Entry = "Idle";
        return g;
    }

    // A file that exists for one test and is removed afterwards.
    class ScratchFile
    {
    public:
        explicit ScratchFile( std::string name )
             : m_Path( std::filesystem::temp_directory_path() / std::move( name ) )
        {
        }

        ~ScratchFile()
        {
            std::error_code ec;
            std::filesystem::remove( m_Path, ec );
        }

        ScratchFile( const ScratchFile& )            = delete;
        ScratchFile& operator=( const ScratchFile& ) = delete;

        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_Path;
        }

        void Write( const std::string& text ) const
        {
            std::ofstream out( m_Path, std::ios::binary | std::ios::trunc );
            out << text;
        }

    private:
        std::filesystem::path m_Path;
    };
} // namespace

TEST( AnimGraphAsset, SaveThenLoadIsTheSameGraph )
{
    const ScratchFile file( "desert_animgraph_roundtrip.danimgraph" );

    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    AnimGraphAsset asset( AssetPriority::Medium, file.Path() );
    const auto     loaded = asset.Load();
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    ASSERT_TRUE( asset.IsReadyForUse() );
    ASSERT_NE( asset.GetGraph(), nullptr );

    EXPECT_EQ( asset.GetGraph()->Name, "Locomotion" );
    EXPECT_EQ( asset.GetGraph()->Entry, "Idle" );
    ASSERT_EQ( asset.GetGraph()->States.size(), 1u );
    EXPECT_EQ( asset.GetGraph()->States[0].Clip, "Anim_Idle" );

    // The DISPLAY name is the graph's own, not the file's stem — the file here is called something else
    // entirely, so a slot showing "desert_animgraph_roundtrip" would mean the wrong half won.
    EXPECT_EQ( asset.GetDisplayName(), "Locomotion" );
}

TEST( AnimGraphAsset, THE_RELATION_OneFileIsOneObjectAndNotACopyPerCaller )
{
    // THIS IS THE ASSERTION THE WHOLE STEP EXISTS FOR. Every entity that names this file gets the SAME
    // pointer, so an edit made in the Anim Graph window is the graph each of them evaluates next frame.
    // Handing out a copy here would compile, pass every round-trip test above, and silently restore the
    // defect: an edit would reach exactly one of the characters using the graph.
    const ScratchFile file( "desert_animgraph_shared.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    AnimGraphAsset asset( AssetPriority::Medium, file.Path() );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    const auto first  = asset.GetGraph();
    const auto second = asset.GetGraph();
    EXPECT_EQ( first.get(), second.get() )
         << "two callers asking this asset for its graph got two different objects";

    // And an edit through one of them is visible through the other, which is the property a copy breaks.
    State extra;
    extra.Name = "Run";
    first->States.push_back( extra );
    EXPECT_EQ( second->States.size(), 2u );
}

TEST( AnimGraphAsset, AnEditBumpsTheRevisionSoEveryEntitySharingItResyncs )
{
    // The counter is the ASSET's, and that is the correction rather than a detail.
    // `AnimationComponent::GraphRevision` used to carry it, bumped by whoever edited — which could only
    // ever be the one component in front of the editor, so a graph shared by two characters would have
    // re-synced ONE of them and left the other evaluating the previous shape with nothing to say so.
    const ScratchFile file( "desert_animgraph_revision.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    AnimGraphAsset asset( AssetPriority::Medium, file.Path() );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    const uint32_t afterLoad = asset.GetRevision();
    asset.MarkEdited();
    EXPECT_GT( asset.GetRevision(), afterLoad ) << "an edit did not move the revision, so no evaluator "
                                                   "anywhere would ever be told the graph changed";

    // A RELOAD moves it too, and does NOT rewind it — a consumer comparing revisions must never see the
    // number go backwards, or an unchanged compare would make a changed graph look current.
    const uint32_t afterEdit = asset.GetRevision();
    ASSERT_TRUE( asset.Load().IsSuccess() );
    EXPECT_GT( asset.GetRevision(), afterEdit );
}

TEST( AnimGraphAsset, AReloadReplacesTheObjectRatherThanRewritingItUnderARunningEvaluator )
{
    // Load() must not mutate the object the running evaluators were built from: they are re-synced on a
    // frame boundary by AnimationECSSystem, and changing the graph mid-evaluation is a state machine
    // whose states move while it is walking them.
    const ScratchFile file( "desert_animgraph_reload.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    AnimGraphAsset asset( AssetPriority::Medium, file.Path() );
    ASSERT_TRUE( asset.Load().IsSuccess() );
    const auto held = asset.GetGraph();

    AnimGraph edited = Locomotion();
    State     run;
    run.Name = "Run";
    edited.States.push_back( run );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), edited ).IsSuccess() );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    EXPECT_NE( asset.GetGraph().get(), held.get() ) << "the reload wrote into the object a live evaluator "
                                                       "was holding";
    EXPECT_EQ( held->States.size(), 1u ) << "the old object changed under its holder";
    EXPECT_EQ( asset.GetGraph()->States.size(), 2u );
}

TEST( AnimGraphAsset, TheObjectItHandsOutIsWhatAnEvaluatorCanBeBuiltFrom )
{
    // The consumer's end of the relation: the shared object is a graph the runtime accepts, not a shape
    // that only the parser is happy with.
    const ScratchFile file( "desert_animgraph_eval.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    AnimGraphAsset asset( AssetPriority::Medium, file.Path() );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    Evaluator evaluator( *asset.GetGraph() );
    evaluator.Reset();
    ASSERT_NE( evaluator.CurrentState(), nullptr );
    EXPECT_EQ( evaluator.CurrentState()->Name, "Idle" );
}

TEST( AnimGraphAsset, AMissingFileIsAnErrorAndNotAnEmptyGraph )
{
    AnimGraphAsset asset( AssetPriority::Medium,
                          std::filesystem::temp_directory_path() / "desert_animgraph_absent.danimgraph" );

    const auto loaded = asset.Load();
    EXPECT_FALSE( loaded.IsSuccess() );
    EXPECT_FALSE( asset.IsReadyForUse() );
    EXPECT_EQ( asset.GetGraph(), nullptr )
         << "a file that is not there produced a graph — the character would stand in an empty state "
            "machine while the scene file plainly names one, which is the silent substitution the "
            "contract forbids";
    EXPECT_NE( loaded.GetError().find( "anim graph" ), std::string::npos ) << loaded.GetError();
}

TEST( AnimGraphAsset, MalformedJsonIsRefusedAndNamesTheFile )
{
    const ScratchFile file( "desert_animgraph_broken.danimgraph" );
    file.Write( "{ this is not json" );

    AnimGraphAsset asset( AssetPriority::Medium, file.Path() );
    const auto     loaded = asset.Load();

    EXPECT_FALSE( loaded.IsSuccess() );
    EXPECT_EQ( asset.GetGraph(), nullptr );
    EXPECT_NE( loaded.GetError().find( file.Path().filename().string() ), std::string::npos )
         << "the refusal does not name the file an author has to go and fix: " << loaded.GetError();
}

TEST( AnimGraphAsset, AnUnloadKeepsTheRevisionMonotonic )
{
    const ScratchFile file( "desert_animgraph_unload.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    AnimGraphAsset asset( AssetPriority::Medium, file.Path() );
    ASSERT_TRUE( asset.Load().IsSuccess() );
    const uint32_t before = asset.GetRevision();

    ASSERT_TRUE( asset.Unload().IsSuccess() );
    EXPECT_EQ( asset.GetGraph(), nullptr );
    EXPECT_GE( asset.GetRevision(), before )
         << "the revision rewound on unload, so the next load would look like no change at all to a "
            "consumer comparing revisions and every entity would keep the graph it had";
}

// ── AND THE CONSUMER'S HALF OF THE RELATION, WHICH NO RUNTIME TEST HERE CAN SEE ───────────────────
//
// The tests above prove the ASSET shares one object. What turns that into "two characters share a graph"
// is one line in AnimationECSSystem::SyncAnimGraph — `anim.Graph = asset->GetGraph()`. Replace it with
// `std::make_shared<AnimGraph>( *asset->GetGraph() )` and everything in this file still passes, every
// entity gets its own copy, and the defect §5.1 removed is back with extra steps. No suite in this
// repository builds two entities on one graph through the ECS, so the relation is asserted over the
// source text — the same instrument MaterialPreviewRoute uses for the same kind of claim.
namespace
{
    std::filesystem::path RepoRoot()
    {
        std::filesystem::path prefix = ".";
        for ( int up = 0; up < 8; ++up )
        {
            if ( std::filesystem::exists( prefix / "Desert/Common/Source/Common/Core/Constants.hpp" ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    }
} // namespace

TEST( AnimGraphAsset, TheEcsHandsOverTheAssetsObjectRatherThanACopyOfIt )
{
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root is not above this working directory";

    // COMMENTS AND LITERALS STRIPPED, through the reader every other source census in this repository
    // uses. Without it the scan below counts the sentence that NAMES the retired field — a census that
    // goes red because somebody explained the thing it is checking for is a census nobody keeps.
    const std::string source = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( root / "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp" ) );
    ASSERT_FALSE( source.empty() );

    const std::size_t sync = source.find( "uint32_t SyncAnimGraph(" );
    ASSERT_NE( sync, std::string::npos ) << "SyncAnimGraph is gone; this test can say nothing about it";
    const std::size_t end = source.find( "\n        }", sync );
    ASSERT_NE( end, std::string::npos );
    const std::string body = source.substr( sync, end - sync );

    EXPECT_NE( body.find( "anim.Graph = asset->GetGraph()" ), std::string::npos )
         << "the ECS no longer hands the entity the ASSET'S OWN graph object. Whatever it hands over "
            "instead, an edit in the Anim Graph window now reaches one entity rather than every entity "
            "naming that file — which is exactly the per-entity blob schema step 21 removed.";
    EXPECT_EQ( body.find( "make_shared<Animation::Graph::AnimGraph>" ), std::string::npos )
         << "SyncAnimGraph is minting a graph of its own. A copy per entity passes every test in this "
            "file and silently restores the defect.";

    // And the revision it reports is the ASSET'S, not a counter on the component.
    EXPECT_NE( body.find( "asset->GetRevision()" ), std::string::npos )
         << "the revision no longer comes from the asset, so a graph shared by two characters can only "
            "re-sync whichever of them the number happens to live on";
    // AND NO COMPONENT-SIDE COUNTER CAME BACK. Spelled as a scan rather than as a `find` compare, because
    // "GraphRevision" is a SUBSTRING of "BuiltGraphRevision" — the obvious one-line version of this check
    // passes on the wrong occurrence, which is the trap this project keeps meeting in string censuses.
    std::size_t bare = 0;
    for ( std::size_t at = source.find( "GraphRevision" ); at != std::string::npos;
          at             = source.find( "GraphRevision", at + 1 ) )
    {
        const bool partOfBuilt = at >= 5 && source.compare( at - 5, 5, "Built" ) == 0;
        if ( !partOfBuilt )
            ++bare;
    }
    EXPECT_EQ( bare, 0u )
         << "a component-side GraphRevision is back — one counter, on the thing that CHANGES, is what "
            "makes a shared graph re-sync every entity that names it; a counter on the component can "
            "only ever re-sync the one in front of the editor";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
