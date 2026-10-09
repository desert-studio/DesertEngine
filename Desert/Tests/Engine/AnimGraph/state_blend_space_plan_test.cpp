// GP2e: PlanPoseGraph's rules for a state that plays a Blend Space 1D (State::BlendSpace) — the loader runs it,
// so each refusal here is a graph that never reaches the Animator.
#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace G = Desert::Animation::Graph;

namespace
{
    /// A machine whose one state "Locomotion" plays Idle 0 / Walk 300 / Run 600 over the Float Speed, and a
    /// Bool "Armed" an axis may not name.
    G::AnimGraph BlendStateGraph()
    {
        G::AnimGraph graph = G::MakeStateMachineGraph( "Hero" );
        graph.Parameters.push_back( { "Speed", static_cast<int>( G::ParamType::Float ), 0.0F } );
        graph.Parameters.push_back( { "Armed", static_cast<int>( G::ParamType::Bool ), 0.0F } );
        G::State state;
        state.Name = "Locomotion";
        state.BlendSpace =
             G::StateBlendSpace{ .Axis  = "Speed",
                                 .Space = G::BlendSpace1DNode{ .Samples     = { G::BlendSample{ "Idle", 0.0F },
                                                                                G::BlendSample{ "Walk", 300.0F },
                                                                                G::BlendSample{ "Run", 600.0F } },
                                                               .WeightSpeed = 4.0F } };
        G::StateMachine* machine = G::OutputMachine( graph );
        machine->Entry           = "Locomotion";
        machine->States.push_back( std::move( state ) );
        return graph;
    }

    G::State& Locomotion( G::AnimGraph& graph )
    {
        return G::OutputMachine( graph )->States.front();
    }

    /// Asserts `graph` is refused with a sentence holding `fragment` and naming the state.
    void ExpectRefused( const G::AnimGraph& graph, const std::string& fragment,
                        G::GraphScope scope = G::GraphScope::Host )
    {
        const auto plan = G::PlanPoseGraph( graph, scope );
        ASSERT_FALSE( plan.IsSuccess() ) << "expected a refusal holding: " << fragment;
        EXPECT_NE( plan.GetError().find( fragment ), std::string::npos ) << plan.GetError();
        EXPECT_NE( plan.GetError().find( "'Locomotion'" ), std::string::npos ) << plan.GetError();
    }
} // namespace

// The control: without it every refusal below could be the graph's fault rather than the rule's.
TEST( StateBlendSpacePlan, ABlendSpaceStateOnADeclaredFloatPlans )
{
    const auto plan = G::PlanPoseGraph( BlendStateGraph() );
    EXPECT_TRUE( plan.IsSuccess() ) << ( plan.IsSuccess() ? std::string() : plan.GetError() );
}

// Red without StateBlendSpaceError's clip check: the state would be planned with two things to play.
TEST( StateBlendSpacePlan, AStateNamingAClipAndABlendSpaceIsRefused )
{
    G::AnimGraph graph       = BlendStateGraph();
    Locomotion( graph ).Clip = "Walk";
    ExpectRefused( graph, "names clip 'Walk' and a blend space" );
}

// Red without the BlendSpace1DError call: a row out of order, with an empty sample or with no sample plans.
TEST( StateBlendSpacePlan, ABadRowIsRefusedWithTheRowsReason )
{
    G::AnimGraph graph                                     = BlendStateGraph();
    Locomotion( graph ).BlendSpace->Space.Samples[2].Value = 100.0F;
    ExpectRefused( graph, "strictly ascending" );

    graph                                                 = BlendStateGraph();
    Locomotion( graph ).BlendSpace->Space.Samples[1].Clip = "";
    ExpectRefused( graph, "names no clip" );

    graph = BlendStateGraph();
    Locomotion( graph ).BlendSpace->Space.Samples.clear();
    ExpectRefused( graph, "no sample" );
}

// Red without the scope check: a layer graph's machine is sampled by its link's clocks, one clip per state.
TEST( StateBlendSpacePlan, ABlendSpaceStateInALayerGraphIsRefused )
{
    ExpectRefused( BlendStateGraph(), "blend spaces play in the host graph only", G::GraphScope::Layer );
}

// Red without the axis check: a Bool or undeclared axis would read 0 forever (the idle sample, silently).
TEST( StateBlendSpacePlan, AnAxisThatIsNoDeclaredFloatIsRefused )
{
    G::AnimGraph graph                   = BlendStateGraph();
    Locomotion( graph ).BlendSpace->Axis = "Armed";
    ExpectRefused( graph, "axis 'Armed' is no declared Float parameter" );

    Locomotion( graph ).BlendSpace->Axis = "Velocity";
    ExpectRefused( graph, "axis 'Velocity' is no declared Float parameter" );
}
