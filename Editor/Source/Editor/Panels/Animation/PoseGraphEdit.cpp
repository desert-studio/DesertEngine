#include "PoseGraphEdit.hpp"

#include <Engine/Animation/Graph/LayeredBlendPerBone.hpp>
#include <Engine/Animation/Graph/LinkedAnimLayer.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

namespace Desert::Editor::Graph
{
    namespace G = Animation::Graph;

    namespace
    {
        int IndexOf( const std::vector<G::PoseNode>& nodes, std::string_view name )
        {
            for ( size_t i = 0; i < nodes.size(); ++i )
                if ( nodes[i].Name == name )
                    return static_cast<int>( i );
            return -1;
        }

        /// The chain of node indices from @p start down its Pose wires to @p target, empty when @p target is not
        /// reached: what a wire target <- start would close into a loop. A walk with its own stack, so a deep
        /// hand-edited chain cannot run the call stack out.
        std::vector<int> PathDown( const std::vector<G::PoseNode>& nodes, int start, int target )
        {
            std::vector<int>  cameFrom( nodes.size(), -1 );
            std::vector<bool> seen( nodes.size(), false );
            std::vector<int>  open{ start };
            seen[static_cast<size_t>( start )] = true;
            while ( !open.empty() )
            {
                const int at = open.back();
                open.pop_back();
                if ( at == target )
                {
                    std::vector<int> path;
                    for ( int step = at; step >= 0; step = cameFrom[static_cast<size_t>( step )] )
                        path.push_back( step );
                    std::reverse( path.begin(), path.end() );
                    return path;
                }
                for ( const std::string& input : nodes[static_cast<size_t>( at )].PoseInputs )
                {
                    const int next = IndexOf( nodes, input );
                    if ( next < 0 || seen[static_cast<size_t>( next )] )
                        continue;
                    seen[static_cast<size_t>( next )]     = true;
                    cameFrom[static_cast<size_t>( next )] = at;
                    open.push_back( next );
                }
            }
            return {};
        }

        constexpr float kPoseGridStepX = 260.0f;
        constexpr float kPoseGridStepY = 150.0f;
        constexpr int   kPoseGridCols  = 4;
    } // namespace

    std::string MakeUniquePoseNodeName( const std::vector<G::PoseNode>& nodes, const std::string& desired )
    {
        const std::string base = desired.empty() ? std::string( "Node" ) : desired;
        std::string       name = base;
        for ( int suffix = 1; IndexOf( nodes, name ) >= 0; ++suffix )
            name = std::format( "{}_{}", base, suffix );
        return name;
    }

    const char* PoseNodeTitle( G::PoseNodeKind kind )
    {
        switch ( kind )
        {
            case G::PoseNodeKind::StateMachine:
                return "State Machine";
            case G::PoseNodeKind::LayeredBlendPerBone:
                return "Layered Blend Per Bone";
            case G::PoseNodeKind::SequencePlayer:
                return "Sequence Player";
            case G::PoseNodeKind::ApplyAdditive:
                return "Apply Additive";
            case G::PoseNodeKind::LinkedAnimLayer:
                return "Linked Anim Layer";
            case G::PoseNodeKind::LinkedInputPose:
                return "Linked Input Pose";
        }
        return "?";
    }

    std::vector<G::PoseNodeKind> AddableKinds( G::GraphScope scope )
    {
        std::vector<G::PoseNodeKind> kinds{ G::PoseNodeKind::SequencePlayer, G::PoseNodeKind::StateMachine,
                                            G::PoseNodeKind::LayeredBlendPerBone, G::PoseNodeKind::ApplyAdditive,
                                            G::PoseNodeKind::LinkedAnimLayer };
        if ( scope == G::GraphScope::Layer )
            kinds.push_back( G::PoseNodeKind::LinkedInputPose );
        return kinds;
    }

    Common::ResultStr<std::string> AddPoseNode( const G::AnimGraph& graph, std::vector<G::PoseNode>& nodes,
                                                G::PoseNodeKind kind, G::GraphScope scope, float x, float y,
                                                const std::string& clip )
    {
        if ( std::string_view( G::KindName( kind ) ) == "?" )
            return Common::MakeError<std::string>(
                 std::format( "no pose node kind {} to add", static_cast<int>( kind ) ) );
        // The engine's rule (PlanPoseGraph refuses it in GraphScope::Host), refused where it is made.
        if ( kind == G::PoseNodeKind::LinkedInputPose && scope != G::GraphScope::Layer )
            return Common::MakeError<std::string>(
                 std::format( "AnimGraph '{}': a Linked Input Pose exists only in a layer graph (it is the pose "
                              "the layer's caller hands in); this is the graph's own AnimGraph",
                              graph.Name ) );

        G::PoseNode node;
        std::string title = PoseNodeTitle( kind );
        std::erase( title, ' ' );
        node.Name = MakeUniquePoseNodeName( nodes, title );
        node.Kind = static_cast<int>( kind );
        node.X    = x;
        node.Y    = y;
        switch ( kind )
        {
            case G::PoseNodeKind::StateMachine:
                node.Machine = G::StateMachine{};
                break;
            case G::PoseNodeKind::LayeredBlendPerBone:
                node.LayeredBlend = G::LayeredBlendPerBoneNode{};
                node.LayeredBlend->Layers.resize( 1 );
                break;
            case G::PoseNodeKind::SequencePlayer:
                node.Sequence = G::SequencePlayerNode{ clip, true };
                break;
            case G::PoseNodeKind::LinkedAnimLayer:
            {
                G::LinkedAnimLayerNode call;
                if ( graph.Layers && !graph.Layers->Interfaces.empty() )
                {
                    call.Interface = graph.Layers->Interfaces.front().Name;
                    if ( !graph.Layers->Interfaces.front().Layers.empty() )
                        call.Layer = graph.Layers->Interfaces.front().Layers.front();
                }
                node.LinkedLayer = call;
                break;
            }
            case G::PoseNodeKind::ApplyAdditive:
            case G::PoseNodeKind::LinkedInputPose:
                break;
        }
        node.PoseInputs.assign( static_cast<size_t>( G::PoseInputCountOf( node ) ), std::string() );
        nodes.push_back( std::move( node ) );
        return Common::MakeSuccess( std::string( nodes.back().Name ) );
    }

    Common::BoolResultStr CanConnectPose( const std::vector<G::PoseNode>& nodes, std::string_view from,
                                          std::string_view to, int pin )
    {
        const int source = IndexOf( nodes, from );
        const int target = IndexOf( nodes, to );
        if ( source < 0 || target < 0 )
            return Common::MakeError<bool>(
                 std::format( "no pose node called '{}' to wire", source < 0 ? from : to ) );
        const G::PoseNode& into = nodes[static_cast<size_t>( target )];
        if ( pin < 0 || pin >= G::PoseInputCountOf( into ) )
            return Common::MakeError<bool>( std::format( "node '{}' ({}) has {} Pose pin(s), no pin {}", into.Name,
                                                         G::KindName( static_cast<G::PoseNodeKind>( into.Kind ) ),
                                                         G::PoseInputCountOf( into ), pin ) );
        if ( source == target )
            return Common::MakeError<bool>(
                 std::format( "node '{}' cannot be wired into itself: a pose graph with a loop has no first node",
                              into.Name ) );

        // The wire makes `to` read `from`; it closes a loop exactly when `from` already reads `to`.
        if ( const std::vector<int> path = PathDown( nodes, source, target ); !path.empty() )
        {
            std::string loop = std::string( to );
            for ( const int step : path ) // `to` reads `from`, which reads down to `to` again
                std::format_to( std::back_inserter( loop ), " -> {}", nodes[static_cast<size_t>( step )].Name );
            return Common::MakeError<bool>( std::format(
                 "wiring '{}' into '{}' would close a cycle in the pose graph: {}. A pose graph with a loop has "
                 "no first node",
                 from, to, loop ) );
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ConnectPose( std::vector<G::PoseNode>& nodes, std::string_view from, std::string_view to,
                                       int pin )
    {
        if ( auto verdict = CanConnectPose( nodes, from, to, pin ); !verdict.IsSuccess() )
            return verdict;
        G::PoseNode& into = nodes[static_cast<size_t>( IndexOf( nodes, to ) )];
        if ( static_cast<int>( into.PoseInputs.size() ) != G::PoseInputCountOf( into ) )
            into.PoseInputs.resize( static_cast<size_t>( G::PoseInputCountOf( into ) ) );
        into.PoseInputs[static_cast<size_t>( pin )] = std::string( from );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ConnectOutput( const std::vector<G::PoseNode>& nodes, std::string& outputPose,
                                         std::string_view from )
    {
        if ( IndexOf( nodes, from ) < 0 )
            return Common::MakeError<bool>(
                 std::format( "no pose node called '{}' to wire into Output Pose", from ) );
        outputPose = std::string( from );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr DisconnectPose( std::vector<G::PoseNode>& nodes, std::string_view to, int pin )
    {
        const int target = IndexOf( nodes, to );
        if ( target < 0 )
            return Common::MakeError<bool>( std::format( "no pose node called '{}' to unwire", to ) );
        G::PoseNode& into = nodes[static_cast<size_t>( target )];
        if ( pin < 0 || pin >= static_cast<int>( into.PoseInputs.size() ) )
            return Common::MakeError<bool>( std::format( "node '{}' has no Pose pin {}", into.Name, pin ) );
        into.PoseInputs[static_cast<size_t>( pin )].clear();
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemovePoseNode( std::vector<G::PoseNode>& nodes, std::string& outputPose,
                                          std::string_view name )
    {
        const int index = IndexOf( nodes, name );
        if ( index < 0 )
            return Common::MakeError<bool>( std::format( "no pose node called '{}' to delete", name ) );
        const std::string gone = nodes[static_cast<size_t>( index )].Name;
        nodes.erase( nodes.begin() + index );
        for ( G::PoseNode& node : nodes )
            for ( std::string& input : node.PoseInputs )
                if ( input == gone )
                    input.clear();
        if ( outputPose == gone )
            outputPose.clear();
        return Common::MakeSuccess( true );
    }

    std::string RenamePoseNode( std::vector<G::PoseNode>& nodes, std::string& outputPose, int index,
                                const std::string& desired )
    {
        G::PoseNode& node = nodes[static_cast<size_t>( index )];
        if ( node.Name == desired )
            return desired;
        const std::string old   = node.Name;
        const std::string given = MakeUniquePoseNodeName( nodes, desired );
        node.Name               = given;
        for ( G::PoseNode& other : nodes )
            for ( std::string& input : other.PoseInputs )
                if ( input == old )
                    input = given;
        if ( outputPose == old )
            outputPose = given;
        return given;
    }

    void SetLayerCount( G::PoseNode& node, size_t count )
    {
        if ( !node.LayeredBlend )
            return;
        const size_t before = node.LayeredBlend->Layers.size();
        node.LayeredBlend->Layers.resize( count );
        node.PoseInputs.resize( static_cast<size_t>( G::PoseInputCountOf( node ) ) );
        for ( size_t layer = count; layer < before; ++layer )
            std::erase_if( node.ParameterInputs, [pin = G::LayerWeightPin( layer )]( const G::ParameterPin& bound )
                           { return bound.Pin == pin; } );
    }

    Common::BoolResultStr BindParameterPin( const G::AnimGraph& graph, G::PoseNode& node, std::string_view pin,
                                            const std::string& parameter )
    {
        if ( !G::HasParameterPin( node, pin ) )
            return Common::MakeError<bool>( std::format( "node '{}' ({}) has no parameter pin '{}'", node.Name,
                                                         G::KindName( static_cast<G::PoseNodeKind>( node.Kind ) ),
                                                         pin ) );
        std::erase_if( node.ParameterInputs, [pin]( const G::ParameterPin& bound ) { return bound.Pin == pin; } );
        if ( parameter.empty() )
            return Common::MakeSuccess( true );
        const bool declared = std::any_of( graph.Parameters.begin(), graph.Parameters.end(),
                                           [&parameter]( const G::Parameter& p ) { return p.Name == parameter; } );
        if ( !declared )
            return Common::MakeError<bool>(
                 std::format( "pin '{}' of '{}' cannot read parameter '{}', which the graph does not declare. It "
                              "declares: {}",
                              pin, node.Name, parameter, G::DeclaredParameterList( graph ) ) );
        node.ParameterInputs.push_back( { std::string( pin ), parameter } );
        return Common::MakeSuccess( true );
    }

    std::string BoundParameter( const G::PoseNode& node, std::string_view pin )
    {
        for ( const G::ParameterPin& bound : node.ParameterInputs )
            if ( bound.Pin == pin )
                return bound.Parameter;
        return {};
    }

    std::string PosePinLabel( const G::PoseNode& node, int pin )
    {
        switch ( static_cast<G::PoseNodeKind>( node.Kind ) )
        {
            case G::PoseNodeKind::LayeredBlendPerBone:
                return pin == 0 ? std::string( "Base Pose" ) : std::format( "Blend Pose {}", pin - 1 );
            case G::PoseNodeKind::ApplyAdditive:
                return pin == 0 ? "Base" : "Additive";
            default:
                return "In Pose";
        }
    }

    PoseGraphCanvas PlanPoseCanvas( const std::vector<G::PoseNode>& nodes, const std::string& outputPose,
                                    ElementIdMap& ids )
    {
        ids.BeginFrame();
        PoseGraphCanvas canvas;
        canvas.OutPins.assign( nodes.size(), ElementId::Invalid );
        canvas.InPins.resize( nodes.size() );

        float rightmost = 0.0f;
        float outputY   = 0.0f;
        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            const G::PoseNode& node     = nodes[i];
            const Resolved     resolved = ids.Resolve( ElementKind::Node, node.Name );
            canvas.OutPins[i]           = ids.Resolve( ElementKind::Pin, std::format( "{}\x1f>", node.Name ) ).Id;
            // The wire slots, not the kind's count: a hand-edited file with too few wires still draws, and
            // the plan's refusal in the strip says what is wrong with it.
            const int pins = std::max( G::PoseInputCountOf( node ), static_cast<int>( node.PoseInputs.size() ) );
            for ( int p = 0; p < pins; ++p )
                canvas.InPins[i].push_back(
                     ids.Resolve( ElementKind::Pin, std::format( "{}\x1f<{}", node.Name, p ) ).Id );

            PlannedNode planned;
            planned.Id           = resolved.Id;
            planned.Key          = node.Name;
            planned.X            = node.X;
            planned.Y            = node.Y;
            planned.PushPosition = resolved.Fresh;
            canvas.Plan.Nodes.push_back( std::move( planned ) );
            rightmost = std::max( rightmost, node.X );
            if ( node.Name == outputPose )
                outputY = node.Y;
        }

        // Output Pose: a sink with no document key of its own; "\x1e" keeps it off every node name.
        const Resolved sink = ids.Resolve( ElementKind::Node, "\x1eOutputPose" );
        canvas.SinkPin      = ids.Resolve( ElementKind::Pin, "\x1eOutputPose\x1f<" ).Id;
        PlannedNode planned;
        planned.Id           = sink.Id;
        planned.Key          = "Output Pose";
        planned.X            = rightmost + kPoseGridStepX;
        planned.Y            = outputY;
        planned.PushPosition = sink.Fresh;
        canvas.Plan.Nodes.push_back( std::move( planned ) );

        const auto addWire = [&]( int from, int to, int pin, ElementId toPin, const std::string& key )
        {
            PlannedLink link;
            link.Id      = ids.Resolve( ElementKind::Link, key ).Id;
            link.Key     = key;
            link.FromPin = Raw( canvas.OutPins[static_cast<size_t>( from )] );
            link.ToPin   = Raw( toPin );
            canvas.Plan.Links.push_back( std::move( link ) );
            canvas.Wires.push_back( { from, to, pin } );
        };
        for ( size_t i = 0; i < nodes.size(); ++i )
            for ( size_t p = 0; p < nodes[i].PoseInputs.size() && p < canvas.InPins[i].size(); ++p )
                if ( const int from = IndexOf( nodes, nodes[i].PoseInputs[p] ); from >= 0 )
                    addWire( from, static_cast<int>( i ), static_cast<int>( p ), canvas.InPins[i][p],
                             std::format( "{}\x1f<{}", nodes[i].Name, p ) );
        if ( const int from = IndexOf( nodes, outputPose ); from >= 0 )
            addWire( from, kOutputSink, 0, canvas.SinkPin, "\x1eOutputPose" );

        ids.EndFrame();
        return canvas;
    }

    PosePinRef PinOf( const PoseGraphCanvas& canvas, uint64_t pin )
    {
        if ( pin == 0 )
            return {};
        if ( pin == Raw( canvas.SinkPin ) )
            return { kOutputSink, 0, false };
        for ( size_t i = 0; i < canvas.OutPins.size(); ++i )
        {
            if ( Raw( canvas.OutPins[i] ) == pin )
                return { static_cast<int>( i ), -1, true };
            for ( size_t p = 0; p < canvas.InPins[i].size(); ++p )
                if ( Raw( canvas.InPins[i][p] ) == pin )
                    return { static_cast<int>( i ), static_cast<int>( p ), false };
        }
        return {};
    }

    int PoseNodeOf( const PoseGraphCanvas& canvas, ElementId node )
    {
        if ( node == ElementId::Invalid )
            return -1;
        for ( size_t i = 0; i < canvas.Plan.Nodes.size(); ++i )
            if ( canvas.Plan.Nodes[i].Id == node )
                return i + 1 == canvas.Plan.Nodes.size() ? kOutputSink : static_cast<int>( i );
        return -1;
    }

    PoseWireRef WireOf( const PoseGraphCanvas& canvas, ElementId link )
    {
        for ( size_t i = 0; i < canvas.Plan.Links.size(); ++i )
            if ( canvas.Plan.Links[i].Id == link )
                return canvas.Wires[i];
        return {};
    }

    std::pair<float, float> NextPoseNodePosition( const std::vector<G::PoseNode>& nodes )
    {
        for ( int cell = 0;; ++cell )
        {
            const int   column = cell % kPoseGridCols;
            const int   row    = cell / kPoseGridCols; // whole rows: the grid is filled row by row
            const float x      = static_cast<float>( column ) * kPoseGridStepX;
            const float y      = static_cast<float>( row ) * kPoseGridStepY;
            const bool  taken  = std::any_of( nodes.begin(), nodes.end(),
                                              [&]( const G::PoseNode& n ) {
                                                return std::abs( n.X - x ) < kPoseGridStepX * 0.5f &&
                                                       std::abs( n.Y - y ) < kPoseGridStepY * 0.5f;
                                            } );
            if ( !taken )
                return { x, y };
        }
    }
} // namespace Desert::Editor::Graph
