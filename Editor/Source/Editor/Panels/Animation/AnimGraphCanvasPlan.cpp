#include "AnimGraphCanvasPlan.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::Editor::Graph
{
    namespace G = ::Desert::Animation::Graph;

    namespace
    {
        // The canvas key of state @p index. A state is named by its `Name` — the same string the runtime
        // resolves `Entry` and `Transition::To` against — so the canvas and the model agree on identity by
        // construction rather than by a second field somebody has to keep in step.
        //
        // THE ORDINAL SUFFIX IS NOT COSMETIC. `MakeUniqueStateName` keeps names unique at every point the
        // editor can create one, but a `.danimgraph` is a text file a person can edit, and a duplicate
        // name reaching this function must still produce two DIFFERENT canvas ids: handing one id to two
        // nodes makes `imgui-node-editor` draw one of them and lose the other. Total function, no
        // precondition on the caller.
        std::string StateKey( const G::AnimGraph& graph, int index )
        {
            const std::string& name = graph.States[static_cast<size_t>( index )].Name;

            int ordinal = 0;
            for ( int i = 0; i < index; ++i )
                if ( graph.States[static_cast<size_t>( i )].Name == name )
                    ++ordinal;

            if ( ordinal == 0 )
                return name;
            return name + "\x1f#" + std::to_string( ordinal + 1 );
        }

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

                const std::string key =
                     TransitionKey( keys[static_cast<size_t>( i )], keys[static_cast<size_t>( target )] );
                const Resolved link = ids.Resolve( ElementKind::Link, key );

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

    std::string MakeUniqueParameterName( const G::AnimGraph& graph, const std::string& desired, int selfIndex )
    {
        const auto taken = [&]( const std::string& candidate )
        {
            for ( int i = 0; i < static_cast<int>( graph.Parameters.size() ); ++i )
            {
                if ( i == selfIndex )
                    continue;
                if ( graph.Parameters[static_cast<size_t>( i )].Name == candidate )
                    return true;
            }
            return false;
        };

        const std::string base = desired.empty() ? std::string( "Param" ) : desired;
        if ( !taken( base ) )
            return base;

        // Bounded by the number of parameters plus one, so the loop cannot fail to find a free name and
        // has no unbounded arm to reason about. The same argument `MakeUniqueStateName` runs.
        for ( size_t suffix = 1; suffix <= graph.Parameters.size() + 1; ++suffix )
        {
            std::string candidate = base + "_" + std::to_string( suffix );
            if ( !taken( candidate ) )
                return candidate;
        }
        return base; // unreachable: N + 1 candidates against at most N occupied names
    }

    std::string RenameParameter( G::AnimGraph& graph, int index, const std::string& desired )
    {
        if ( index < 0 || index >= static_cast<int>( graph.Parameters.size() ) )
            return {};

        const std::string previous = graph.Parameters[static_cast<size_t>( index )].Name;
        const std::string renamed  = MakeUniqueParameterName( graph, desired, index );
        graph.Parameters[static_cast<size_t>( index )].Name = renamed;

        if ( renamed == previous )
            return renamed;

        // WHICH PARAMETER THE CONDITIONS WERE ACTUALLY READING. `Evaluator::FindParameter` takes the first
        // declaration carrying the name, so conditions on `previous` belong to the first parameter of that
        // name and to no other. A later duplicate — which only a hand-edited file can produce, since both
        // creation and rename go through `MakeUniqueParameterName` — must leave them where they are.
        for ( int i = 0; i < index; ++i )
        {
            if ( graph.Parameters[static_cast<size_t>( i )].Name == previous )
                return renamed;
        }

        for ( auto& state : graph.States )
        {
            for ( auto& transition : state.Transitions )
            {
                for ( auto& condition : transition.Conditions )
                {
                    if ( condition.Parameter == previous )
                        condition.Parameter = renamed;
                }
            }
        }
        return renamed;
    }

    StatePosition NextStatePosition( const G::AnimGraph& graph )
    {
        // Half a step in each axis. A cell is "taken" when an existing state sits closer to its centre
        // than that, which is the same thing as saying the two nodes would visually collide.
        const auto occupied = [&]( const StatePosition& cell )
        {
            for ( const auto& state : graph.States )
            {
                if ( std::abs( state.X - cell.X ) < kStateGridStepX * 0.5f &&
                     std::abs( state.Y - cell.Y ) < kStateGridStepY * 0.5f )
                {
                    return true;
                }
            }
            return false;
        };

        // BOUNDED BY N + 1, and that bound is a proof rather than a guess: a point lies within half a
        // step of at most one grid centre per axis, so N states can take at most N cells, and one of the
        // first N + 1 cells is therefore free. The same argument `MakeUniqueStateName` runs.
        const int cells = static_cast<int>( graph.States.size() ) + 1;
        for ( int cell = 0; cell < cells; ++cell )
        {
            const StatePosition candidate{ static_cast<float>( cell % kStateGridColumns ) * kStateGridStepX,
                                           static_cast<float>( cell / kStateGridColumns ) * kStateGridStepY };
            if ( !occupied( candidate ) )
                return candidate;
        }
        return {}; // unreachable, by the bound above
    }
} // namespace Desert::Editor::Graph
