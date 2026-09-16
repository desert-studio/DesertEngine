// CANVAS IDENTITY — AND THE ONE DEFECT THAT COULD ONLY BE SEEN FROM OUTSIDE A UI CONTEXT.
//
// ── WHAT THIS SUITE IS PAYING FOR ────────────────────────────────────────────────────────────────────
//
// `imgui-node-editor` remembers where a node sits UNDER THE ID THE PANEL DREW IT WITH. The anim graph's
// panel computed that id from a position in a vector:
//
//     AnimGraphPanel.cpp:50-53   NodeId( i )            = i + 1
//     AnimGraphPanel.cpp:62-65   LinkId( state, trans ) = kLink + state * 4096 + trans
//
// Delete state 1 of 6 and every later state shifts down one index. On the NEXT frame state i is drawn
// under the id that used to belong to state i+1, `ed::GetNodePosition` returns that neighbour's position,
// and `AnimGraphPanel.cpp:315-320` writes it into `State.X/Y`. One deletion moves the layout of every
// state after it, the file is then saved that way, and nothing anywhere says a word.
//
// It was reasoned about in `Docs/Animation/07_panels_design.md` §5.2 and never measured, for a reason
// worth stating: it lives inside a draw call, behind a live `ed::EditorContext`, which no build machine
// has. That is exactly why the identity layer and both documents' plan builders are units of their own
// now — `Editor/Core/GraphCanvas/` and the two `*CanvasPlan.cpp` — with no ImGui in them at all.
//
// ── HOW THE DEFECT IS MEASURED HERE ──────────────────────────────────────────────────────────────────
//
// `CanvasPositionStore` below is a faithful model of the ONE piece of node-editor state the defect runs
// through, written from the vendor's own source and not from its documentation:
//
//   * `SetNodePosition(id, p)`      stores p under id                  (imgui_node_editor.cpp:1669-1674)
//   * a node DRAWN under an unknown id appears at the origin
//   * `GetNodePosition(unknown id)` is (FLT_MAX, FLT_MAX)              (imgui_node_editor.cpp:1676-1683)
//
// A "frame" is then the panel's own loop: push the stored position for anything the canvas has never
// seen, draw, read the position back into the model. Nothing about it is invented — it is the sequence
// `AnimGraphPanel::DrawCanvas` performs, with the plan builder supplying the ids.
//
// ── AND WHY BOTH GRAPHS ARE IN ONE SUITE ─────────────────────────────────────────────────────────────
//
// The shared layer is only worth having if a break in it is visible from both sides. `ElementIdMap` is
// built on `ElementLedger`; the shader graph uses the ledger directly (it issues its own ids and always
// did, correctly). So `TheFreshnessRuleIsOneRuleForBothGraphs` drives BOTH documents through the same
// helper, and `FingerprintOf*` prints an FNV-1a per shipped graph — the same instrument
// `ShaderGraphDeterminism` uses on emitted DSL, pointed at what a frame would submit.

#include <gtest/gtest.h>

#include <Editor/Core/GraphCanvas/GraphCanvas.hpp>
#include <Editor/Panels/Animation/AnimGraphCanvasPlan.hpp>
#include <Editor/Panels/NodeGraph/ShaderGraphCanvasPlan.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/Serialization/ShaderGraph.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace G   = Desert::Animation::Graph;
namespace GC  = Desert::Editor::Graph;
namespace SGF = Desert::Assets::Serialization::ShaderGraph;

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

    struct Position
    {
        float X = 0.0f;
        float Y = 0.0f;

        bool operator==( const Position& other ) const
        {
            return X == other.X && Y == other.Y;
        }
    };

    /// The node editor's position store, modelled from its source. See the file note for the three
    /// behaviours and where each of them is written.
    class CanvasPositionStore
    {
    public:
        void SetNodePosition( GC::ElementId id, Position position )
        {
            m_Positions[GC::Raw( id )] = position;
        }

        /// Drawing a node the canvas has never heard of creates it at the origin.
        void DrawNode( GC::ElementId id )
        {
            m_Positions.emplace( GC::Raw( id ), Position{ 0.0f, 0.0f } );
        }

        [[nodiscard]] Position GetNodePosition( GC::ElementId id ) const
        {
            const auto hit = m_Positions.find( GC::Raw( id ) );
            if ( hit == m_Positions.end() )
                return Position{ FLT_MAX, FLT_MAX };
            return hit->second;
        }

    private:
        std::map<uint64_t, Position> m_Positions;
    };

    /// One frame of `AnimGraphPanel::DrawCanvas`, with everything that is not identity removed. Returns
    /// the plan it drew, because asking for a second plan to inspect would CONSUME the freshness this
    /// layer is about — a frame is a frame, and there is exactly one per frame.
    GC::AnimGraphCanvas DrawFrame( G::AnimGraph& graph, GC::ElementIdMap& ids, CanvasPositionStore& canvas )
    {
        const GC::AnimGraphCanvas planned = GC::PlanAnimGraph( graph, ids );

        for ( size_t i = 0; i < graph.States.size(); ++i )
        {
            const GC::PlannedNode& node = planned.Plan.Nodes[i];

            if ( node.PushPosition )
                canvas.SetNodePosition( node.Id, Position{ node.X, node.Y } );

            canvas.DrawNode( node.Id );

            const Position back = canvas.GetNodePosition( node.Id );
            graph.States[i].X   = back.X;
            graph.States[i].Y   = back.Y;
        }
        return planned;
    }

    G::AnimGraph GraphWithStates( int count )
    {
        G::AnimGraph graph;
        for ( int i = 0; i < count; ++i )
        {
            G::State state;
            state.Name = "S" + std::to_string( i );
            state.X    = 100.0f * static_cast<float>( i + 1 );
            state.Y    = 10.0f * static_cast<float>( i + 1 );
            graph.States.push_back( state );
        }
        graph.Entry = graph.States.empty() ? "" : graph.States.front().Name;
        return graph;
    }

    std::string PanelSource( const char* relative )
    {
        return ReadAll( RepoRoot() / relative );
    }
} // namespace

// ── 1. THE DEFECT, AS A NUMBER ───────────────────────────────────────────────────────────────────────

TEST( GraphCanvasIdentity, DeletingOneStateMovesNoneOfTheOthers )
{
    G::AnimGraph        graph = GraphWithStates( 6 );
    GC::ElementIdMap    ids;
    CanvasPositionStore canvas;

    ( void )DrawFrame( graph, ids, canvas ); // the document opens: every stored position goes in

    std::vector<Position> before;
    for ( const auto& state : graph.States )
        before.push_back( Position{ state.X, state.Y } );

    // The user deletes the second state. Everything after it shifts down one index in the model — which
    // is a fact about a vector and must not be a fact about the canvas.
    graph.States.erase( graph.States.begin() + 1 );
    before.erase( before.begin() + 1 );

    ( void )DrawFrame( graph, ids, canvas );

    int moved = 0;
    for ( size_t i = 0; i < graph.States.size(); ++i )
    {
        const Position now{ graph.States[i].X, graph.States[i].Y };
        if ( !( now == before[i] ) )
        {
            ++moved;
            std::printf( "[GraphCanvasIdentity] '%s' moved (%.1f,%.1f) -> (%.1f,%.1f)\n",
                         graph.States[i].Name.c_str(), before[i].X, before[i].Y, now.X, now.Y );
        }
    }

    // FIVE STATES SURVIVE AND NOT ONE OF THEM MAY HAVE MOVED. With `NodeId( i ) = i + 1` this number is
    // 4: every state after the deleted one inherits its neighbour's position, and the last one — drawn
    // under an id the canvas has never seen — reads back (FLT_MAX, FLT_MAX).
    EXPECT_EQ( moved, 0 ) << moved << " of " << graph.States.size()
                          << " states were moved by deleting a DIFFERENT state";
}

TEST( GraphCanvasIdentity, ADeletedStateTakesItsIdWithIt )
{
    G::AnimGraph     graph = GraphWithStates( 3 );
    GC::ElementIdMap ids;

    const GC::AnimGraphCanvas first  = GC::PlanAnimGraph( graph, ids );
    const GC::ElementId       secondId = first.StateNodes[1];
    const GC::ElementId       thirdId  = first.StateNodes[2];

    graph.States.erase( graph.States.begin() + 1 );

    const GC::AnimGraphCanvas second = GC::PlanAnimGraph( graph, ids );

    // The survivor keeps the id it had; the deleted state's id is not handed to anybody.
    EXPECT_EQ( second.StateNodes[1], thirdId );
    EXPECT_NE( second.StateNodes[1], secondId );
    EXPECT_EQ( GC::StateOfNode( second, secondId ), -1 );
    EXPECT_EQ( GC::StateOfNode( second, thirdId ), 1 );
}

// ── 2. SELECTION AND DELETION NAME THE ELEMENT, NOT THE SLOT ─────────────────────────────────────────

TEST( GraphCanvasIdentity, DeletingATransitionDoesNotRenumberTheOthers )
{
    G::AnimGraph graph = GraphWithStates( 4 );
    graph.States[0].Transitions.push_back( G::Transition{ "S1", 0.2f, false, 1.0f, {} } );
    graph.States[0].Transitions.push_back( G::Transition{ "S2", 0.2f, false, 1.0f, {} } );
    graph.States[0].Transitions.push_back( G::Transition{ "S3", 0.2f, false, 1.0f, {} } );

    GC::ElementIdMap          ids;
    const GC::AnimGraphCanvas first = GC::PlanAnimGraph( graph, ids );
    ASSERT_EQ( first.Plan.Links.size(), 3u );

    const GC::ElementId toS3 = first.Plan.Links[2].Id;
    ASSERT_EQ( GC::TransitionOfLink( first, toS3 ).Index, 2 );

    // Delete S0 -> S1. `kLink + state * 4096 + index` would now decode `toS3` as S0 -> S3's OLD slot,
    // which after the erase holds nothing; the next click would edit a different transition.
    graph.States[0].Transitions.erase( graph.States[0].Transitions.begin() );

    const GC::AnimGraphCanvas second = GC::PlanAnimGraph( graph, ids );
    const GC::TransitionRef   ref    = GC::TransitionOfLink( second, toS3 );

    ASSERT_TRUE( ref.Valid() );
    EXPECT_EQ( ref.State, 0 );
    EXPECT_EQ( ref.Index, 1 ); // it moved down one SLOT and is still the same transition
    EXPECT_EQ( graph.States[ref.State].Transitions[ref.Index].To, "S3" );
}

TEST( GraphCanvasIdentity, IdsDoNotCollideAcrossKindsOrAtScale )
{
    // `state * 4096 + transition` collides at 4096 transitions on one state, and the node range started
    // at 1 while pins started at 0x2000'0000 — three ranges chosen by hand and never checked against one
    // another. 120 states with 60 transitions each is 7200 links, well past where the old encoding wraps.
    G::AnimGraph graph = GraphWithStates( 120 );
    for ( auto& state : graph.States )
        for ( int t = 0; t < 60; ++t )
            state.Transitions.push_back( G::Transition{ "S" + std::to_string( t ), 0.2f, false, 1.0f, {} } );

    GC::ElementIdMap          ids;
    const GC::AnimGraphCanvas planned = GC::PlanAnimGraph( graph, ids );

    std::set<uint64_t> seen;
    const auto         claim = [&]( GC::ElementId id )
    {
        EXPECT_NE( id, GC::ElementId::Invalid );
        EXPECT_TRUE( seen.insert( GC::Raw( id ) ).second ) << "id " << GC::Raw( id ) << " issued twice";
    };

    for ( size_t i = 0; i < graph.States.size(); ++i )
    {
        claim( planned.StateNodes[i] );
        claim( planned.StateInPins[i] );
        claim( planned.StateOutPins[i] );
    }
    for ( const auto& link : planned.Plan.Links )
        claim( link.Id );

    EXPECT_EQ( planned.Plan.Links.size(), 7200u );
    EXPECT_EQ( seen.size(), 120u * 3u + 7200u );

    // Every id must also be in the range its kind claims, or `KindOf` — which the panels use to tell a
    // pin from a node — answers about a number rather than about an element.
    for ( size_t i = 0; i < graph.States.size(); ++i )
    {
        EXPECT_EQ( GC::KindOf( planned.StateNodes[i] ), GC::ElementKind::Node );
        EXPECT_EQ( GC::KindOf( planned.StateInPins[i] ), GC::ElementKind::Pin );
        EXPECT_EQ( GC::KindOf( planned.StateOutPins[i] ), GC::ElementKind::Pin );
    }
    for ( const auto& link : planned.Plan.Links )
        EXPECT_EQ( GC::KindOf( link.Id ), GC::ElementKind::Link );
}

// ── 3. THE HOLE `m_ApplyPositions` LEFT ──────────────────────────────────────────────────────────────

TEST( GraphCanvasIdentity, AStateAddedAfterTheFirstFrameStillGetsItsStoredPosition )
{
    G::AnimGraph        graph = GraphWithStates( 2 );
    GC::ElementIdMap    ids;
    CanvasPositionStore canvas;

    ( void )DrawFrame( graph, ids, canvas );

    G::State late;
    late.Name = "Late";
    late.X    = 777.0f;
    late.Y    = 555.0f;
    graph.States.push_back( late );

    const GC::AnimGraphCanvas planned = DrawFrame( graph, ids, canvas );
    EXPECT_FALSE( planned.Plan.Nodes[0].PushPosition );
    EXPECT_FALSE( planned.Plan.Nodes[1].PushPosition );
    EXPECT_TRUE( planned.Plan.Nodes[2].PushPosition ) << "the canvas has never seen this element";

    EXPECT_FLOAT_EQ( graph.States[2].X, 777.0f );
    EXPECT_FLOAT_EQ( graph.States[2].Y, 555.0f );
}

// ── 4. A DUPLICATE NAME IS STILL TWO ELEMENTS ────────────────────────────────────────────────────────

TEST( GraphCanvasIdentity, TwoStatesSharingANameGetTwoIds )
{
    // A `.danimgraph` is a text file, so this can arrive from disk however carefully the editor keeps
    // names unique. Handing one id to two nodes makes the canvas draw one of them and lose the other.
    G::AnimGraph graph = GraphWithStates( 2 );
    graph.States[1].Name = graph.States[0].Name;

    GC::ElementIdMap          ids;
    const GC::AnimGraphCanvas planned = GC::PlanAnimGraph( graph, ids );

    EXPECT_NE( planned.StateNodes[0], planned.StateNodes[1] );
    EXPECT_EQ( GC::StateOfNode( planned, planned.StateNodes[0] ), 0 );
    EXPECT_EQ( GC::StateOfNode( planned, planned.StateNodes[1] ), 1 );
}

TEST( GraphCanvasIdentity, TheEditorCannotCreateADuplicateName )
{
    G::AnimGraph graph = GraphWithStates( 3 ); // S0 S1 S2

    EXPECT_EQ( GC::MakeUniqueStateName( graph, "S1", -1 ), "S1_1" );
    EXPECT_EQ( GC::MakeUniqueStateName( graph, "S1", 1 ), "S1" ); // renaming a state to its own name
    EXPECT_EQ( GC::MakeUniqueStateName( graph, "S1", 2 ), "S1_1" );
    EXPECT_EQ( GC::MakeUniqueStateName( graph, "Fresh", -1 ), "Fresh" );
    EXPECT_EQ( GC::MakeUniqueStateName( graph, "", -1 ), "State" );
}

// ── 5. THE FRESHNESS RULE IS ONE RULE, AND BOTH GRAPHS RUN IT ────────────────────────────────────────

namespace
{
    /// "Is this element new to the canvas?" asked of whatever a document uses to answer it. Both
    /// documents are driven through this one helper so that a break in `ElementLedger` — which
    /// `ElementIdMap` is built on — is visible from both sides rather than from the anim graph alone.
    struct FreshnessRun
    {
        std::vector<bool> FirstFrame;
        std::vector<bool> SecondFrame;
        std::vector<bool> AfterOneWasReplaced;
    };

    FreshnessRun RunAnimGraph()
    {
        G::AnimGraph     graph = GraphWithStates( 3 );
        GC::ElementIdMap ids;

        FreshnessRun run;
        for ( const auto& node : GC::PlanAnimGraph( graph, ids ).Plan.Nodes )
            run.FirstFrame.push_back( node.PushPosition );
        for ( const auto& node : GC::PlanAnimGraph( graph, ids ).Plan.Nodes )
            run.SecondFrame.push_back( node.PushPosition );

        graph.States[1].Name = "Renamed";
        for ( const auto& node : GC::PlanAnimGraph( graph, ids ).Plan.Nodes )
            run.AfterOneWasReplaced.push_back( node.PushPosition );
        return run;
    }

    FreshnessRun RunShaderGraph()
    {
        SGF::Document     doc;
        GC::ElementLedger ledger;
        for ( uint64_t i = 0; i < 3; ++i )
        {
            SGF::Node node;
            node.Id   = doc.NextId++;
            node.Kind = "FloatConst";
            doc.Nodes.push_back( node );
        }

        FreshnessRun run;
        for ( const auto& node : GC::PlanShaderGraph( doc, ledger ).Nodes )
            run.FirstFrame.push_back( node.PushPosition );
        for ( const auto& node : GC::PlanShaderGraph( doc, ledger ).Nodes )
            run.SecondFrame.push_back( node.PushPosition );

        doc.Nodes[1].Id = doc.NextId++; // the same slot, a different element
        for ( const auto& node : GC::PlanShaderGraph( doc, ledger ).Nodes )
            run.AfterOneWasReplaced.push_back( node.PushPosition );
        return run;
    }

    void ExpectTheOneRule( const char* who, const FreshnessRun& run )
    {
        SCOPED_TRACE( who );
        EXPECT_EQ( run.FirstFrame, ( std::vector<bool>{ true, true, true } ) )
             << "a cold canvas knows nothing, so every element must be pushed in";
        EXPECT_EQ( run.SecondFrame, ( std::vector<bool>{ false, false, false } ) )
             << "telling the canvas a position it already has is how a node becomes undraggable";
        EXPECT_EQ( run.AfterOneWasReplaced, ( std::vector<bool>{ false, true, false } ) )
             << "only the element that was replaced is new";
    }
} // namespace

TEST( GraphCanvasIdentity, TheFreshnessRuleIsOneRuleForBothGraphs )
{
    const FreshnessRun anim   = RunAnimGraph();
    const FreshnessRun shader = RunShaderGraph();

    ExpectTheOneRule( "anim graph", anim );
    ExpectTheOneRule( "shader graph", shader );

    // AND THE TWO ANSWERS ARE THE SAME ANSWER. Asserting each separately would still pass if the two
    // documents grew two rules that happened to agree on this case; this line is the relation.
    EXPECT_EQ( anim.FirstFrame, shader.FirstFrame );
    EXPECT_EQ( anim.SecondFrame, shader.SecondFrame );
    EXPECT_EQ( anim.AfterOneWasReplaced, shader.AfterOneWasReplaced );
}

// ── 6. THE FINGERPRINT, OVER THE GRAPHS THIS PROJECT SHIPS ───────────────────────────────────────────

TEST( GraphCanvasIdentity, EveryCommittedAnimGraphPlansToTheSameCanvasTwice )
{
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::filesystem::path folder = root / "Editor/Resources/Assets/AnimGraphs";
    ASSERT_TRUE( std::filesystem::exists( folder ) ) << folder.generic_string();

    std::vector<std::filesystem::path> files;
    for ( const auto& entry : std::filesystem::directory_iterator( folder ) )
        if ( entry.path().extension() == G::kAnimGraphExtension )
            files.push_back( entry.path() );
    std::sort( files.begin(), files.end() );

    ASSERT_FALSE( files.empty() ) << "no .danimgraph in " << folder.generic_string();

    for ( const auto& file : files )
    {
        const auto parsed = G::Deserialize( ReadAll( file ) );
        ASSERT_TRUE( parsed.IsSuccess() ) << file.filename().string() << ": " << parsed.GetError();

        GC::ElementIdMap  firstIds;
        GC::ElementIdMap  secondIds;
        const G::AnimGraph graph = parsed.GetValue();

        const uint64_t a = GC::Fingerprint( GC::PlanAnimGraph( graph, firstIds ).Plan );
        const uint64_t b = GC::Fingerprint( GC::PlanAnimGraph( graph, secondIds ).Plan );

        std::printf( "[GraphCanvasIdentity] %-28s canvas fnv1a=%016llx\n",
                     file.filename().string().c_str(), static_cast<unsigned long long>( a ) );

        EXPECT_EQ( a, b ) << file.filename().string() << " planned to two different canvases";
    }
}

TEST( GraphCanvasIdentity, EveryCommittedShaderGraphPlansToTheSameCanvasTwice )
{
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::filesystem::path> files;
    for ( const auto& entry :
          std::filesystem::recursive_directory_iterator( root / "Editor/Resources/Assets" ) )
        if ( entry.path().extension() == ".dgraph" )
            files.push_back( entry.path() );
    std::sort( files.begin(), files.end() );

    ASSERT_FALSE( files.empty() ) << "no .dgraph under Editor/Resources/Assets";

    for ( const auto& file : files )
    {
        const auto parsed = SGF::ParseShaderGraph( ReadAll( file ) );
        ASSERT_TRUE( parsed.IsSuccess() ) << file.filename().string() << ": " << parsed.GetError();

        GC::ElementLedger  firstLedger;
        GC::ElementLedger  secondLedger;
        const SGF::Document doc = parsed.GetValue();

        const uint64_t a = GC::Fingerprint( GC::PlanShaderGraph( doc, firstLedger ) );
        const uint64_t b = GC::Fingerprint( GC::PlanShaderGraph( doc, secondLedger ) );

        std::printf( "[GraphCanvasIdentity] %-28s canvas fnv1a=%016llx\n",
                     file.filename().string().c_str(), static_cast<unsigned long long>( a ) );

        EXPECT_EQ( a, b ) << file.filename().string() << " planned to two different canvases";
    }
}

TEST( GraphCanvasIdentity, TheFingerprintNoticesWhatItIsFor )
{
    // A NEGATIVE CONTROL FOR THE INSTRUMENT ABOVE. Two graphs that a fingerprint cannot tell apart is a
    // fingerprint that proves nothing, and the three things this layer owns are exactly identity, order
    // and position.
    G::AnimGraph     graph = GraphWithStates( 3 );
    GC::ElementIdMap ids;
    const uint64_t   base = GC::Fingerprint( GC::PlanAnimGraph( graph, ids ).Plan );

    {
        G::AnimGraph     moved = GraphWithStates( 3 );
        moved.States[1].X += 1.0f;
        GC::ElementIdMap other;
        EXPECT_NE( GC::Fingerprint( GC::PlanAnimGraph( moved, other ).Plan ), base ) << "position";
    }
    {
        G::AnimGraph     renamed = GraphWithStates( 3 );
        renamed.States[1].Name   = "Other";
        GC::ElementIdMap other;
        EXPECT_NE( GC::Fingerprint( GC::PlanAnimGraph( renamed, other ).Plan ), base ) << "identity";
    }
    {
        G::AnimGraph reordered = GraphWithStates( 3 );
        std::swap( reordered.States[0], reordered.States[2] );
        GC::ElementIdMap other;
        EXPECT_NE( GC::Fingerprint( GC::PlanAnimGraph( reordered, other ).Plan ), base ) << "order";
    }
    {
        G::AnimGraph linked = GraphWithStates( 3 );
        linked.States[0].Transitions.push_back( G::Transition{ "S1", 0.2f, false, 1.0f, {} } );
        GC::ElementIdMap other;
        EXPECT_NE( GC::Fingerprint( GC::PlanAnimGraph( linked, other ).Plan ), base ) << "links";
    }
}

TEST( GraphCanvasIdentity, FramingWaitsForTheFrameAfterTheFirst )
{
    // The canvas does not exist on the first `ed::Begin` — see `DeferredFrameAll`'s declaration for the
    // measurement. Framing on the frame it is asked for is exactly what the anim graph did, and it is
    // why its view was unrecoverable.
    GC::DeferredFrameAll deferred;
    EXPECT_FALSE( deferred.Tick() ) << "the first drawn frame's canvas may not be there at all";
    EXPECT_TRUE( deferred.Tick() );
    EXPECT_FALSE( deferred.Tick() ) << "framing every frame would fight the user's own panning";
    EXPECT_FALSE( deferred.Tick() );

    deferred.Request();
    EXPECT_FALSE( deferred.Tick() );
    EXPECT_TRUE( deferred.Tick() );
    EXPECT_FALSE( deferred.Tick() );
}

// ── 7. THE CENSUS: WHAT MUST NOT COME BACK ───────────────────────────────────────────────────────────

TEST( GraphCanvasIdentity, NeitherPanelDerivesACanvasIdFromAnIndex )
{
    // A CENSUS OVER THE SOURCE TEXT, because the outcome tests above can only see the rule the plan
    // builders use. A panel that went back to computing ids in its draw call would pass all of them and
    // reintroduce the defect, and this is the shape rather than the symptom.
    const std::pair<const char*, const char*> panels[] = {
         { "anim graph", "Editor/Source/Editor/Panels/Animation/AnimGraphPanel.cpp" },
         { "shader graph", "Editor/Source/Editor/Panels/NodeGraph/NodeGraphPanel.cpp" },
    };

    // `ed::NodeId( <something> + 1 )` and the 4096-wide link encoding, in code rather than in a comment:
    // a line that is not a comment and that builds a node/pin/link id out of arithmetic.
    const std::regex arithmeticId( R"(ed::(NodeId|PinId|LinkId)\s*\([^)]*[+*])" );

    for ( const auto& [who, relative] : panels )
    {
        SCOPED_TRACE( who );
        const std::string source = PanelSource( relative );
        ASSERT_FALSE( source.empty() ) << relative;

        std::istringstream lines( source );
        std::string        line;
        int                lineNumber = 0;
        while ( std::getline( lines, line ) )
        {
            ++lineNumber;
            const size_t firstGlyph = line.find_first_not_of( " \t" );
            if ( firstGlyph != std::string::npos && line.compare( firstGlyph, 2, "//" ) == 0 )
                continue; // the defect is DESCRIBED in both files, deliberately

            EXPECT_FALSE( std::regex_search( line, arithmeticId ) )
                 << relative << ":" << lineNumber << " computes a canvas id: " << line;
        }
    }
}

TEST( GraphCanvasIdentity, BothGraphDocumentsOfferFrameAllWithoutAMouse )
{
    // WHY THIS IS PINNED. A canvas whose view goes wrong and that cannot be re-framed is a document that
    // is over: that is literally what happened to the anim graph, which had no `Frame All` and drew an
    // empty rectangle for its whole life once `imgui-node-editor`'s lazy first-frame init had refused its
    // canvas. And it has to be a DOCUMENT ACTION, not only a button: synthetic input is closed on this
    // machine, so a button is a control no script and no test can press.
    for ( const char* relative : { "Editor/Source/Editor/Panels/Animation/AnimGraphPanel.cpp",
                                   "Editor/Source/Editor/Panels/NodeGraph/NodeGraphPanel.cpp" } )
    {
        SCOPED_TRACE( relative );
        const std::string source = PanelSource( relative );
        ASSERT_FALSE( source.empty() ) << relative;
        EXPECT_NE( source.find( R"({ "Frame All", [this] { Graph::FrameAll( m_Context ); } },)" ),
                   std::string::npos )
             << "no Frame All among this document's actions";
        EXPECT_NE( source.find( R"({ "Frame Selection", [this] { Graph::FrameSelection( m_Context ); } },)" ),
                   std::string::npos )
             << "no Frame Selection among this document's actions";
    }
}

TEST( GraphCanvasIdentity, NeitherCanvasIsWrappedInAChildWindow )
{
    // THE MEASURED CAUSE OF THE EMPTY CANVAS. `imgui-node-editor` initialises lazily inside the first
    // `ed::Begin` by running a throwaway Canvas Begin/End, and `Canvas::End` emits a dummy widget the
    // size of the canvas — so the REAL Begin of that frame starts one canvas lower and is refused
    // whenever the enclosing window's clip rect ends there. Measured on this tree: the anim graph's
    // canvas, inside a child sized exactly to it, landed at y=965 against a clip rect ending at y=962
    // and was refused; the shader graph's, drawn into the document window, landed at y=965 against 969
    // and survived by four pixels.
    //
    // `DeferredFrameAll` makes that first frame harmless. This census keeps the trigger away as well,
    // because a first frame that draws nothing is still a first frame that draws nothing.
    for ( const char* relative : { "Editor/Source/Editor/Panels/Animation/AnimGraphPanel.cpp",
                                   "Editor/Source/Editor/Panels/NodeGraph/NodeGraphPanel.cpp" } )
    {
        SCOPED_TRACE( relative );
        const std::string source = PanelSource( relative );
        ASSERT_FALSE( source.empty() ) << relative;

        const size_t canvasBegin = source.find( "ed::Begin(" );
        ASSERT_NE( canvasBegin, std::string::npos ) << "this panel draws no canvas at all";

        // The child the anim graph used to open around its canvas, by its own name. Both panels are
        // allowed children — the side inspector is one — but not one that contains `ed::Begin`.
        const size_t child = source.rfind( "ImGui::BeginChild", canvasBegin );
        if ( child == std::string::npos )
            continue;
        const size_t endChild = source.rfind( "ImGui::EndChild", canvasBegin );
        EXPECT_TRUE( endChild != std::string::npos && endChild > child )
             << "ed::Begin is inside an unclosed ImGui::BeginChild";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
