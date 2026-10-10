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
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
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

namespace
{
    AnimGraph Locomotion()
    {
        AnimGraph g = ::Desert::Animation::Graph::MakeStateMachineGraph();
        g.Name = "Locomotion";
        g.TargetSkeleton = { "fedcba9876543210fedcba9876543210", "Meshes/Locomotion.skeleton" };
        State idle;
        idle.Name = "Idle";
        idle.Clip = "Anim_Idle";
        OutputMachine( g )->States.push_back( idle );
        OutputMachine( g )->Entry = "Idle";
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

    AnimGraphAsset asset( file.Path() );
    const auto     loaded = asset.Load();
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    ASSERT_TRUE( asset.IsReadyForUse() );
    ASSERT_NE( asset.GetGraph(), nullptr );

    EXPECT_EQ( asset.GetGraph()->Name, "Locomotion" );
    EXPECT_EQ( OutputMachine( *asset.GetGraph() )->Entry, "Idle" );
    ASSERT_EQ( OutputMachine( *asset.GetGraph() )->States.size(), 1u );
    EXPECT_EQ( OutputMachine( *asset.GetGraph() )->States[0].Clip, "Anim_Idle" );

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

    AnimGraphAsset asset( file.Path() );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    const auto first  = asset.GetGraph();
    const auto second = asset.GetGraph();
    EXPECT_EQ( first.get(), second.get() )
         << "two callers asking this asset for its graph got two different objects";

    // And an edit through one of them is visible through the other, which is the property a copy breaks.
    State extra;
    extra.Name = "Run";
    OutputMachine( *first )->States.push_back( extra );
    EXPECT_EQ( OutputMachine( *second )->States.size(), 2u );
}

TEST( AnimGraphAsset, AnEditBumpsTheRevisionSoEveryEntitySharingItResyncs )
{
    // The counter is the ASSET's, and that is the correction rather than a detail.
    // `AnimationComponent::GraphRevision` used to carry it, bumped by whoever edited — which could only
    // ever be the one component in front of the editor, so a graph shared by two characters would have
    // re-synced ONE of them and left the other evaluating the previous shape with nothing to say so.
    const ScratchFile file( "desert_animgraph_revision.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    AnimGraphAsset asset( file.Path() );
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

    AnimGraphAsset asset( file.Path() );
    ASSERT_TRUE( asset.Load().IsSuccess() );
    const auto held = asset.GetGraph();

    AnimGraph edited = Locomotion();
    State     run;
    run.Name = "Run";
    OutputMachine( edited )->States.push_back( run );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), edited ).IsSuccess() );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    EXPECT_NE( asset.GetGraph().get(), held.get() ) << "the reload wrote into the object a live evaluator "
                                                       "was holding";
    EXPECT_EQ( OutputMachine( *held )->States.size(), 1u ) << "the old object changed under its holder";
    EXPECT_EQ( OutputMachine( *asset.GetGraph() )->States.size(), 2u );
}

TEST( AnimGraphAsset, TheObjectItHandsOutIsWhatAnEvaluatorCanBeBuiltFrom )
{
    // The consumer's end of the relation: the shared object is a graph the runtime accepts, not a shape
    // that only the parser is happy with.
    const ScratchFile file( "desert_animgraph_eval.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    AnimGraphAsset asset( file.Path() );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    Evaluator evaluator( *asset.GetGraph() );
    evaluator.Reset();
    ASSERT_NE( evaluator.CurrentState(), nullptr );
    EXPECT_EQ( evaluator.CurrentState()->Name, "Idle" );
}

TEST( AnimGraphAsset, AMissingFileIsAnErrorAndNotAnEmptyGraph )
{
    AnimGraphAsset asset( std::filesystem::temp_directory_path() / "desert_animgraph_absent.danimgraph" );

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

    AnimGraphAsset asset( file.Path() );
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

    AnimGraphAsset asset( file.Path() );
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

// THE TEXT ASSET HEADER (T7d, ANGR 1): a save states it, a resave keeps its GUID, the handle IS that GUID's,
// and a generation-0 file (no header) is refused by name, pointing at the migrator.
TEST( AnimGraphAsset, ASaveStatesTheHeaderAndAResaveKeepsItsGuid )
{
    const ScratchFile file( "desert_animgraph_header.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), Locomotion() ).IsSuccess() );

    const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
    const auto first = Common::Content::ReadAssetHeader( file.Path(), recordOnly );
    ASSERT_TRUE( first ) << first.GetError();
    EXPECT_EQ( first.GetValue().Kind, Common::Content::ContentKind::AnimGraph );
    ASSERT_FALSE( first.GetValue().Guid.IsNull() );
    ASSERT_EQ( first.GetValue().Subsystems.size(), 1u );
    EXPECT_EQ( first.GetValue().Subsystems[0].Tag, Desert::Assets::kAnimGraphSchemaTag );
    EXPECT_EQ( first.GetValue().Subsystems[0].Version, Desert::Assets::kAnimGraphSchemaVersion )
         << "a save states the layout this build writes";

    AnimGraphAsset asset( file.Path() );
    EXPECT_EQ( static_cast<uint64_t>( asset.GetMetadata().Handle ),
               static_cast<uint64_t>( Common::Content::HandleForGuid( first.GetValue().Guid ) ) )
         << "the constructor did not adopt the header GUID";
    ASSERT_TRUE( asset.Load().IsSuccess() );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), *asset.GetGraph() ).IsSuccess() );

    const auto second = Common::Content::ReadAssetHeader( file.Path(), recordOnly );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_EQ( second.GetValue().Guid, first.GetValue().Guid ) << "a resave minted a second identity";
}

TEST( AnimGraphAsset, AGraphWithNoHeaderIsRefusedByNameAndPointsAtTheMigrator )
{
    const ScratchFile file( "desert_animgraph_gen0.danimgraph" );
    file.Write( R"({"Name":"Old","Entry":"Idle","Parameters":[],"States":[{"Name":"Idle","Clip":"A"}]})" );

    AnimGraphAsset asset( file.Path() );
    const auto     loaded = asset.Load();
    ASSERT_FALSE( loaded.IsSuccess() );
    EXPECT_EQ( asset.GetGraph(), nullptr );
    EXPECT_NE( loaded.GetError().find( "format version 0" ), std::string::npos ) << loaded.GetError();
    EXPECT_NE( loaded.GetError().find( "SceneMigrator" ), std::string::npos ) << loaded.GetError();
}

// The linked-layer half of a graph (ANIM-I14) survives the file; a file without it is a graph without layers.
TEST( AnimGraphAsset, LayerInterfacesAndImplementedLayersRoundTrip )
{
    namespace PG       = Desert::Animation::Graph;
    AnimGraph    graph = PG::MakeStateMachineGraph( "Rifle" );
    graph.TargetSkeleton = { "fedcba9876543210fedcba9876543210", "Meshes/Locomotion.skeleton" };
    PG::PoseNode input;
    input.Name   = "In";
    input.Kind   = static_cast<int>( PG::PoseNodeKind::LinkedInputPose );
    graph.Layers = PG::AnimGraphLayers{ { PG::AnimLayerInterface{ "Weapon", { "UpperBody" } } },
                                        { PG::AnimLayerGraph{ "Weapon", "UpperBody", { input }, "In" } } };

    const auto read = PG::Deserialize( PG::Serialize( graph ) );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const auto& layers = read.GetValue().Layers;
    if ( !layers )
    {
        FAIL() << "the layers did not survive the round trip";
    }
    ASSERT_EQ( layers->Implemented.size(), 1u );
    EXPECT_EQ( layers->Interfaces[0].Layers, std::vector<std::string>{ "UpperBody" } );
    EXPECT_EQ( layers->Implemented[0].Nodes[0].Kind, input.Kind );

    AnimGraph plainGraph      = PG::MakeStateMachineGraph( "Plain" );
    plainGraph.TargetSkeleton = graph.TargetSkeleton;
    const auto plain          = PG::Deserialize( PG::Serialize( plainGraph ) );
    ASSERT_TRUE( plain.IsSuccess() ) << plain.GetError();
    EXPECT_FALSE( plain.GetValue().Layers.has_value() );
}

// A graph that names no skeleton is refused on read: the binding is part of the asset (ANIM-SKELREF).
TEST( AnimGraphAsset, GraphWithoutTargetSkeletonIsRefused )
{
    namespace PG    = Desert::Animation::Graph;
    const auto read = PG::Deserialize( PG::Serialize( PG::MakeStateMachineGraph( "NoSkeleton" ) ) );
    EXPECT_FALSE( read.IsSuccess() );
}

// ANIM-FIX10: the skeletal control nodes' setups (Two Bone IK, Look At) survive the .danimgraph file, targets,
// space bones and Alpha included.
TEST( AnimGraphAsset, TwoBoneIKAndLookAtNodesRoundTrip )
{
    namespace PG         = Desert::Animation::Graph;
    AnimGraph graph      = PG::MakeStateMachineGraph( "Reach" );
    graph.TargetSkeleton = { "fedcba9876543210fedcba9876543210", "Meshes/Locomotion.skeleton" };

    PG::PoseNode ik;
    ik.Name                     = "IK";
    ik.Kind                     = static_cast<int>( PG::PoseNodeKind::TwoBoneIK );
    ik.PoseInputs               = { graph.OutputPose };
    ik.TwoBoneIK                = PG::TwoBoneIKNode{};
    ik.TwoBoneIK->EndBone       = "Hand";
    ik.TwoBoneIK->Goal.Position = { 30.0F, 70.0F, 20.0F };
    ik.TwoBoneIK->PoleTarget    = PG::BoneControlTarget{ { 0.0F, 0.0F, 50.0F }, "Spine" };
    ik.TwoBoneIK->Alpha         = 0.25F;
    PG::PoseNode look;
    look.Name                    = "Look";
    look.Kind                    = static_cast<int>( PG::PoseNodeKind::LookAt );
    look.PoseInputs              = { "IK" };
    look.LookAt                  = PG::LookAtNode{};
    look.LookAt->Bone            = "Head";
    look.LookAt->Target.Position = { 1.0F, 2.0F, 3.0F };
    look.LookAt->AimAxis         = { 1.0F, 0.0F, 0.0F };
    graph.Nodes.push_back( ik );
    graph.Nodes.push_back( look );
    graph.OutputPose = "Look";

    const auto read = PG::Deserialize( PG::Serialize( graph ) );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    ASSERT_EQ( read.GetValue().Nodes.size(), 3u );
    const PG::PoseNode& readIK   = read.GetValue().Nodes[1];
    const PG::PoseNode& readLook = read.GetValue().Nodes[2];
    ASSERT_TRUE( readIK.TwoBoneIK.has_value() );
    EXPECT_EQ( readIK.TwoBoneIK->EndBone, "Hand" );
    EXPECT_EQ( readIK.TwoBoneIK->Goal.Position, ik.TwoBoneIK->Goal.Position );
    EXPECT_EQ( readIK.TwoBoneIK->PoleTarget.Bone, "Spine" );
    EXPECT_EQ( readIK.TwoBoneIK->Alpha, 0.25F );
    ASSERT_TRUE( readLook.LookAt.has_value() );
    EXPECT_EQ( readLook.LookAt->Bone, "Head" );
    EXPECT_EQ( readLook.LookAt->AimAxis, look.LookAt->AimAxis );
}

// ANIM-FIX7: comment boxes (UE: UEdGraphNode_Comment) belong to their canvas - the pose graph, a layer graph,
// a state machine - and survive Save/Load with their id, title, place and size on each of the three.
TEST( AnimGraphAsset, CommentBoxesOfEveryCanvasSurviveSaveAndLoad )
{
    namespace PG       = Desert::Animation::Graph;
    AnimGraph    graph = PG::MakeStateMachineGraph( "Commented" );
    graph.TargetSkeleton = { "fedcba9876543210fedcba9876543210", "Meshes/Locomotion.skeleton" };
    PG::PoseNode input;
    input.Name   = "In";
    input.Kind   = static_cast<int>( PG::PoseNodeKind::LinkedInputPose );
    graph.Layers = PG::AnimGraphLayers{ { PG::AnimLayerInterface{ "Weapon", { "UpperBody" } } },
                                        { PG::AnimLayerGraph{ "Weapon", "UpperBody", { input }, "In" } } };
    (void)PG::AddComment( graph.Comments, "Locomotion", -40.0f, -60.0f, 420.0f, 260.0f );
    (void)PG::AddComment( graph.Comments, "Aim", 500.0f, 10.0f, 200.0f, 120.0f );
    (void)PG::AddComment( graph.Layers->Implemented[0].Comments, "Upper body", 5.0f, 6.0f, 70.0f, 80.0f );
    (void)PG::AddComment( OutputMachine( graph )->Comments, "Ground states", 1.5f, 2.5f, 300.0f, 150.0f );
    ASSERT_EQ( graph.Comments[1].Id, 2u ) << "AddComment issues an id no comment of the canvas carries";

    const ScratchFile file( "desert_animgraph_comments.danimgraph" );
    ASSERT_TRUE( AnimGraphAsset::Save( file.Path(), graph ).IsSuccess() );
    AnimGraphAsset asset( file.Path() );
    const auto     loaded = asset.Load();
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    const AnimGraph& back = *asset.GetGraph();

    ASSERT_EQ( back.Comments.size(), 2u );
    EXPECT_EQ( back.Comments[0].Id, 1u );
    EXPECT_EQ( back.Comments[0].Text, "Locomotion" );
    EXPECT_FLOAT_EQ( back.Comments[0].X, -40.0f );
    EXPECT_FLOAT_EQ( back.Comments[0].Y, -60.0f );
    EXPECT_FLOAT_EQ( back.Comments[0].Width, 420.0f );
    EXPECT_FLOAT_EQ( back.Comments[0].Height, 260.0f );
    EXPECT_EQ( back.Comments[1].Text, "Aim" );
    ASSERT_TRUE( back.Layers.has_value() );
    ASSERT_EQ( back.Layers->Implemented[0].Comments.size(), 1u );
    EXPECT_EQ( back.Layers->Implemented[0].Comments[0].Text, "Upper body" );
    EXPECT_FLOAT_EQ( back.Layers->Implemented[0].Comments[0].Height, 80.0f );
    const auto* machine = OutputMachine( back );
    ASSERT_NE( machine, nullptr );
    ASSERT_EQ( machine->Comments.size(), 1u );
    EXPECT_EQ( machine->Comments[0].Text, "Ground states" );
    EXPECT_FLOAT_EQ( machine->Comments[0].X, 1.5f );
    EXPECT_FLOAT_EQ( machine->Comments[0].Width, 300.0f );

    // An id is never reissued while a higher one lives: deleting the first box and adding one does not
    // hand the new box the survivor's id (the canvas keys the box by it).
    std::vector<PG::GraphComment> comments = back.Comments;
    comments.erase( comments.begin() );
    EXPECT_EQ( PG::AddComment( comments, "New", 0.0f, 0.0f, 1.0f, 1.0f ).Id, 3u );
}
