#include "AnimGraphCanvasPlan.hpp"

#include <algorithm>

namespace Desert::Editor::Graph
{
    namespace G = ::Desert::Animation::Graph;

    namespace
    {
        // THE CANVAS KEY OF STATE @p index, AS THE TREE COMPUTES IT TODAY — the position of the state in
        // `graph.States`. `AnimGraphPanel.cpp:50-53` spelt it `NodeId( i ) = i + 1`; the arithmetic has
        // moved here unchanged, and the id it yields is the same id.
        //
        // THIS IS THE DEFECT AND IT IS DELIBERATELY STILL HERE for one commit, so that the suite that
        // measures it can be seen going red over the rule the tree actually ships rather than over a
        // replica of it written inside a test. The next commit replaces this one function.
        std::string StateKey( const G::AnimGraph& graph, int index )
        {
            ( void )graph;
            return std::to_string( index );
        }

        // Likewise `LinkId( state, transition ) = kLink + state * 4096 + transition`, by position.
        std::string TransitionKey( const std::string& from, const std::string& to )
        {
            return from + "\x1f>" + to;
        }

        // THE SAME RULE `Evaluator::FindState` USES: the FIRST state carrying the name. A canvas that
        // drew the link to a different duplicate than the runtime follows would be a picture of a graph
        // nobody is running.
        int FindStateByName( const G::AnimGraph& graph, const std::string& name )
        {
            for ( int i = 0; i < static_cast<int>( graph.States.size() ); ++i )
                if ( graph.States[static_cast<size_t>( i )].Name == name )
                    return i;
            return -1;
        }
    } // namespace

    AnimGraphCanvas PlanAnimGraph( const G::AnimGraph& graph, ElementIdMap& ids )
    {
        ids.BeginFrame();

        AnimGraphCanvas canvas;

        const auto stateCount = static_cast<int>( graph.States.size() );
        canvas.StateNodes.assign( static_cast<size_t>( stateCount ), ElementId::Invalid );
        canvas.StateInPins.assign( static_cast<size_t>( stateCount ), ElementId::Invalid );
        canvas.StateOutPins.assign( static_cast<size_t>( stateCount ), ElementId::Invalid );

        std::vector<std::string> keys;
        keys.reserve( static_cast<size_t>( stateCount ) );

        for ( int i = 0; i < stateCount; ++i )
        {
            const auto& state = graph.States[static_cast<size_t>( i )];
            keys.push_back( StateKey( graph, i ) );
            const std::string& key = keys.back();

            const Resolved node = ids.Resolve( ElementKind::Node, key );
            const Resolved in   = ids.Resolve( ElementKind::Pin, key + "\x1f<" );
            const Resolved out  = ids.Resolve( ElementKind::Pin, key + "\x1f>" );

            canvas.StateNodes[static_cast<size_t>( i )]   = node.Id;
            canvas.StateInPins[static_cast<size_t>( i )]  = in.Id;
            canvas.StateOutPins[static_cast<size_t>( i )] = out.Id;

            PlannedNode planned;
            planned.Id           = node.Id;
            planned.Key          = key;
            planned.X            = state.X;
            planned.Y            = state.Y;
            planned.PushPosition = node.Fresh;
            canvas.Plan.Nodes.push_back( std::move( planned ) );
        }

        for ( int i = 0; i < stateCount; ++i )
        {
            const auto& state = graph.States[static_cast<size_t>( i )];
            for ( int t = 0; t < static_cast<int>( state.Transitions.size() ); ++t )
            {
                const int target = FindStateByName( graph, state.Transitions[static_cast<size_t>( t )].To );
                if ( target < 0 )
                    continue; // a transition to a name no state carries has nothing to draw between

                const std::string key = TransitionKey( keys[static_cast<size_t>( i )],
                                                       keys[static_cast<size_t>( target )] );
                const Resolved    link = ids.Resolve( ElementKind::Link, key );

                PlannedLink planned;
                planned.Id      = link.Id;
                planned.Key     = key;
                planned.FromPin = Raw( canvas.StateOutPins[static_cast<size_t>( i )] );
                planned.ToPin   = Raw( canvas.StateInPins[static_cast<size_t>( target )] );
                canvas.Plan.Links.push_back( std::move( planned ) );
                canvas.LinkRefs.push_back( TransitionRef{ i, t } );
            }
        }

        ids.EndFrame();
        return canvas;
    }

    int StateOfNode( const AnimGraphCanvas& canvas, ElementId node )
    {
        if ( node == ElementId::Invalid )
            return -1;
        const auto hit = std::find( canvas.StateNodes.begin(), canvas.StateNodes.end(), node );
        return hit == canvas.StateNodes.end() ? -1 : static_cast<int>( hit - canvas.StateNodes.begin() );
    }

    int StateOfInPin( const AnimGraphCanvas& canvas, uint64_t pin )
    {
        if ( pin == 0 )
            return -1;
        const auto id  = static_cast<ElementId>( pin );
        const auto hit = std::find( canvas.StateInPins.begin(), canvas.StateInPins.end(), id );
        return hit == canvas.StateInPins.end() ? -1 : static_cast<int>( hit - canvas.StateInPins.begin() );
    }

    int StateOfOutPin( const AnimGraphCanvas& canvas, uint64_t pin )
    {
        if ( pin == 0 )
            return -1;
        const auto id  = static_cast<ElementId>( pin );
        const auto hit = std::find( canvas.StateOutPins.begin(), canvas.StateOutPins.end(), id );
        return hit == canvas.StateOutPins.end() ? -1 : static_cast<int>( hit - canvas.StateOutPins.begin() );
    }

    TransitionRef TransitionOfLink( const AnimGraphCanvas& canvas, ElementId link )
    {
        for ( size_t i = 0; i < canvas.Plan.Links.size(); ++i )
            if ( canvas.Plan.Links[i].Id == link )
                return canvas.LinkRefs[i];
        return {};
    }

    std::string MakeUniqueStateName( const G::AnimGraph& graph, const std::string& desired, int selfIndex )
    {
        const auto taken = [&]( const std::string& candidate )
        {
            for ( int i = 0; i < static_cast<int>( graph.States.size() ); ++i )
            {
                if ( i == selfIndex )
                    continue;
                if ( graph.States[static_cast<size_t>( i )].Name == candidate )
                    return true;
            }
            return false;
        };

        const std::string base = desired.empty() ? std::string( "State" ) : desired;
        if ( !taken( base ) )
            return base;

        // Bounded by the number of states plus one, so the loop cannot fail to find a free name and has
        // no unbounded arm to reason about.
        for ( size_t suffix = 1; suffix <= graph.States.size() + 1; ++suffix )
        {
            std::string candidate = base + "_" + std::to_string( suffix );
            if ( !taken( candidate ) )
                return candidate;
        }
        return base; // unreachable: N+1 candidates against at most N occupied names
    }
} // namespace Desert::Editor::Graph
