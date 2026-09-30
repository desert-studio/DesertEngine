#include "AnimGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <utility>

// The pose graph's structure: node kinds and their pins, lookups, and the evaluation plan. Kept apart from
// the Evaluator because the loader (AnimGraphSerialization.cpp) refuses an unplannable graph and links no
// runtime.
namespace Desert::Animation::Graph
{
    const char* TypeName( ParamType type )
    {
        switch ( type )
        {
            case ParamType::Bool:
                return "Bool";
            case ParamType::Int:
                return "Int";
            case ParamType::Float:
                return "Float";
        }
        return "?";
    }

    std::string DeclaredParameterList( const AnimGraph& graph )
    {
        std::string declared;
        for ( const auto& p : graph.Parameters )
        {
            declared += declared.empty() ? "" : ", ";
            declared += std::format( "'{}' ({})", p.Name, TypeName( static_cast<ParamType>( p.Type ) ) );
        }
        return declared.empty() ? std::string( "none at all" ) : declared;
    }

    const char* KindName( PoseNodeKind kind )
    {
        switch ( kind )
        {
            case PoseNodeKind::StateMachine:
                return "StateMachine";
        }
        return "?";
    }

    PoseNodePins PinsOf( PoseNodeKind kind )
    {
        switch ( kind )
        {
            case PoseNodeKind::StateMachine:
                // A state machine produces its pose from its states' clips: no Pose input, and its
                // conditions read the graph's parameters directly rather than through pins.
                return PoseNodePins{ .PoseInputs = 0, .ParameterPins = {} };
        }
        return PoseNodePins{};
    }

    AnimGraph MakeStateMachineGraph( std::string name )
    {
        AnimGraph graph;
        graph.Name = std::move( name );

        PoseNode machine;
        machine.Name    = std::string( kDefaultStateMachineNode );
        machine.Kind    = static_cast<int>( PoseNodeKind::StateMachine );
        machine.Machine = StateMachine{};
        graph.Nodes.push_back( std::move( machine ) );
        graph.OutputPose = std::string( kDefaultStateMachineNode );
        return graph;
    }

    const PoseNode* FindNode( const AnimGraph& graph, std::string_view name )
    {
        const auto it = std::find_if( graph.Nodes.begin(), graph.Nodes.end(),
                                      [name]( const PoseNode& node ) { return node.Name == name; } );
        return it == graph.Nodes.end() ? nullptr : &*it;
    }

    PoseNode* FindNode( AnimGraph& graph, std::string_view name )
    {
        return const_cast<PoseNode*>( FindNode( std::as_const( graph ), name ) );
    }

    const StateMachine* OutputMachine( const AnimGraph& graph )
    {
        const PoseNode* output = FindNode( graph, graph.OutputPose );
        if ( output == nullptr || static_cast<PoseNodeKind>( output->Kind ) != PoseNodeKind::StateMachine ||
             !output->Machine )
            return nullptr;
        return &*output->Machine;
    }

    StateMachine* OutputMachine( AnimGraph& graph )
    {
        return const_cast<StateMachine*>( OutputMachine( std::as_const( graph ) ) );
    }

    namespace
    {
        /// Every per-node rule that does not need the wires followed: the name, the kind, the pins, the
        /// payload. The first broken rule, as a sentence naming the node; empty when the node is sound.
        std::string NodeError( const AnimGraph& graph, const PoseNode& node )
        {
            if ( node.Name.empty() )
                return std::format( "AnimGraph '{}' has a pose node with no name; wires name nodes, so a "
                                    "nameless node can be wired by nothing",
                                    graph.Name );

            const auto kind = static_cast<PoseNodeKind>( node.Kind );
            if ( std::string_view( KindName( kind ) ) == "?" )
                return std::format( "AnimGraph '{}': node '{}' is of kind {}, which no pose node kind is",
                                    graph.Name, node.Name, node.Kind );

            const PoseNodePins pins = PinsOf( kind );
            if ( static_cast<int>( node.PoseInputs.size() ) != pins.PoseInputs )
                return std::format( "AnimGraph '{}': node '{}' ({}) has {} Pose pin(s) and {} wire(s) into them",
                                    graph.Name, node.Name, KindName( kind ), pins.PoseInputs,
                                    node.PoseInputs.size() );

            for ( const ParameterPin& bound : node.ParameterInputs )
            {
                if ( std::find( pins.ParameterPins.begin(), pins.ParameterPins.end(), bound.Pin ) ==
                     pins.ParameterPins.end() )
                    return std::format( "AnimGraph '{}': node '{}' ({}) binds a parameter to pin '{}', which a "
                                        "{} node does not have",
                                        graph.Name, node.Name, KindName( kind ), bound.Pin, KindName( kind ) );
                const bool declared =
                     std::any_of( graph.Parameters.begin(), graph.Parameters.end(),
                                  [&bound]( const Parameter& p ) { return p.Name == bound.Parameter; } );
                if ( !declared )
                    return std::format( "AnimGraph '{}': node '{}' binds pin '{}' to parameter '{}', which the "
                                        "graph does not declare. It declares: {}",
                                        graph.Name, node.Name, bound.Pin, bound.Parameter,
                                        DeclaredParameterList( graph ) );
            }

            if ( kind == PoseNodeKind::StateMachine && !node.Machine )
                return std::format( "AnimGraph '{}': node '{}' is a StateMachine node with no machine in it",
                                    graph.Name, node.Name );
            if ( kind != PoseNodeKind::StateMachine && node.Machine )
                return std::format( "AnimGraph '{}': node '{}' ({}) carries a state machine, which only a "
                                    "StateMachine node has",
                                    graph.Name, node.Name, KindName( kind ) );
            return {};
        }
    } // namespace

    Common::ResultStr<std::vector<int>> PlanPoseGraph( const AnimGraph& graph )
    {
        using Plan = std::vector<int>;

        for ( size_t i = 0; i < graph.Nodes.size(); ++i )
        {
            if ( std::string error = NodeError( graph, graph.Nodes[i] ); !error.empty() )
                return Common::MakeError<Plan>( std::move( error ) );
            for ( size_t j = 0; j < i; ++j )
                if ( graph.Nodes[j].Name == graph.Nodes[i].Name )
                    return Common::MakeError<Plan>(
                         std::format( "AnimGraph '{}' has two pose nodes called '{}'; a wire to that name would "
                                      "mean either",
                                      graph.Name, graph.Nodes[i].Name ) );
        }

        const auto indexOf = [&graph]( std::string_view name ) -> int
        {
            for ( size_t i = 0; i < graph.Nodes.size(); ++i )
                if ( graph.Nodes[i].Name == name )
                    return static_cast<int>( i );
            return -1;
        };

        const int output = indexOf( graph.OutputPose );
        if ( output < 0 )
            return Common::MakeError<Plan>(
                 graph.OutputPose.empty()
                      ? std::format( "AnimGraph '{}' has nothing wired into Output Pose", graph.Name )
                      : std::format( "AnimGraph '{}' wires node '{}' into Output Pose, and has no such node",
                                     graph.Name, graph.OutputPose ) );

        // Depth-first from the output through the Pose pins, post-order: a node is placed after every node
        // wired into it. `path` is the chain being walked; meeting a node on it again is a cycle.
        enum class Mark : uint8_t
        {
            Unvisited,
            OnPath,
            Placed
        };
        std::vector<Mark> marks( graph.Nodes.size(), Mark::Unvisited );
        std::vector<int>  path;
        Plan              plan;
        std::string       error;

        const auto visit = [&]( const auto& self, int node ) -> bool
        {
            marks[node] = Mark::OnPath;
            path.push_back( node );
            for ( const std::string& wired : graph.Nodes[node].PoseInputs )
            {
                const int input = indexOf( wired );
                if ( input < 0 )
                {
                    error = std::format( "AnimGraph '{}': node '{}' has a Pose pin wired to '{}', and the graph "
                                         "has no such node",
                                         graph.Name, graph.Nodes[node].Name, wired );
                    return false;
                }
                if ( marks[input] == Mark::OnPath )
                {
                    std::string loop;
                    const auto  from = std::find( path.begin(), path.end(), input );
                    for ( auto it = from; it != path.end(); ++it )
                        loop += std::format( "{} -> ", graph.Nodes[*it].Name );
                    loop += graph.Nodes[input].Name;
                    error = std::format( "AnimGraph '{}' has a cycle in its pose graph: {}. A pose graph "
                                         "evaluates each node after its inputs, and a loop has no first node",
                                         graph.Name, loop );
                    return false;
                }
                if ( marks[input] == Mark::Unvisited && !self( self, input ) )
                    return false;
            }
            path.pop_back();
            marks[node] = Mark::Placed;
            plan.push_back( node );
            return true;
        };

        if ( !visit( visit, output ) )
            return Common::MakeError<Plan>( std::move( error ) );
        return Common::MakeSuccess( std::move( plan ) );
    }
} // namespace Desert::Animation::Graph
