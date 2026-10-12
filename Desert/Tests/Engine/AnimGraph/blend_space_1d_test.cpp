// GP2d: the Blend Space 1D node (UE UBlendSpace1D on a Blend Space Player) — its weights on the axis, the weight
// smoothing (TargetWeightInterpolationSpeedPerSec), the phase sync of its samples and the plan's refusals.
#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numeric>

namespace G = Desert::Animation::Graph;

namespace
{
    // UE-like locomotion row in cm/s: Idle at 0, Walk at 300, Run at 600.
    G::BlendSpace1DNode LocomotionRow()
    {
        return G::BlendSpace1DNode{ .Samples = { G::BlendSample{ "Idle", 0.0F }, G::BlendSample{ "Walk", 300.0F },
                                                 G::BlendSample{ "Run", 600.0F } } };
    }

    std::array<float, 3> WeightsAt( float x )
    {
        std::array<float, 3> w{};
        G::BlendSpace1DTargetWeights( LocomotionRow(), x, w );
        return w;
    }

    float Sum( const std::array<float, 3>& w )
    {
        return std::accumulate( w.begin(), w.end(), 0.0F );
    }
} // namespace

TEST( BlendSpace1D, ASampleValueGivesThatSampleTheWholeWeight )
{
    EXPECT_EQ( WeightsAt( 0.0F ), ( std::array<float, 3>{ 1.0F, 0.0F, 0.0F } ) );
    EXPECT_EQ( WeightsAt( 300.0F ), ( std::array<float, 3>{ 0.0F, 1.0F, 0.0F } ) );
    EXPECT_EQ( WeightsAt( 600.0F ), ( std::array<float, 3>{ 0.0F, 0.0F, 1.0F } ) );
}

TEST( BlendSpace1D, BetweenTwoSamplesTheyShareTheWeightLinearlyAndSumToOne )
{
    const auto quarter = WeightsAt( 75.0F ); // a quarter of the way from Idle to Walk
    EXPECT_NEAR( quarter[0], 0.75F, 1e-6F );
    EXPECT_NEAR( quarter[1], 0.25F, 1e-6F );
    EXPECT_EQ( quarter[2], 0.0F );
    const auto runward = WeightsAt( 450.0F );
    EXPECT_EQ( runward[0], 0.0F );
    EXPECT_NEAR( runward[1], 0.5F, 1e-6F );
    EXPECT_NEAR( runward[2], 0.5F, 1e-6F );
    for ( float x = -50.0F; x <= 700.0F; x += 37.0F )
        EXPECT_NEAR( Sum( WeightsAt( x ) ), 1.0F, 1e-6F ) << "at " << x;
}

TEST( BlendSpace1D, AnAxisOutsideTheRowIsClampedToItsEndSamples )
{
    EXPECT_EQ( WeightsAt( -120.0F ), ( std::array<float, 3>{ 1.0F, 0.0F, 0.0F } ) );
    EXPECT_EQ( WeightsAt( 2000.0F ), ( std::array<float, 3>{ 0.0F, 0.0F, 1.0F } ) );
}

TEST( BlendSpace1D, WeightSpeedMovesEachWeightAtMostThatMuchPerSecondAndKeepsTheSumOne )
{
    std::array<float, 3>       weights{ 1.0F, 0.0F, 0.0F };
    const std::array<float, 3> target{ 0.0F, 0.0F, 1.0F };
    G::InterpolateBlendWeights( weights, target, 2.0F, 0.1F ); // 0.2 of weight in this step
    EXPECT_NEAR( weights[0], 0.8F / 1.0F, 1e-5F );
    EXPECT_NEAR( weights[2], 0.2F / 1.0F, 1e-5F );
    EXPECT_NEAR( weights[0] + weights[1] + weights[2], 1.0F, 1e-6F );
    for ( int i = 0; i < 10; ++i )
        G::InterpolateBlendWeights( weights, target, 2.0F, 0.1F );
    EXPECT_EQ( weights, target ) << "a second at 2/s reaches the target";

    std::array<float, 3> snapped{ 1.0F, 0.0F, 0.0F };
    G::InterpolateBlendWeights( snapped, target, 0.0F, 0.016F );
    EXPECT_EQ( snapped, target ) << "WeightSpeed 0 is no smoothing";
}

// Two cycles of different length blended half and half: the shared phase is what both samples play at, so the
// normalized times stay equal (the feet stay in step), and the blend's cycle lasts the weight-averaged length.
TEST( BlendSpace1D, PhaseSyncKeepsTwoCyclesOfDifferentLengthAtOneNormalizedTime )
{
    const std::array<float, 2> weights{ 0.5F, 0.5F };
    const std::array<float, 2> lengths{ 1.0F, 0.6F }; // walk 1.0 s, run 0.6 s -> blended cycle 0.8 s
    float                      phase = 0.0F;
    for ( int frame = 0; frame < 24; ++frame )
        phase = G::AdvanceSyncedPhase( phase, weights, lengths, 1.0F / 60.0F, true );
    EXPECT_NEAR( phase, 0.4F / 0.8F, 1e-4F );
    // Each sample's own normalized time is the phase: its seconds over its length agree.
    const float walkSeconds = phase * lengths[0];
    const float runSeconds  = phase * lengths[1];
    EXPECT_NEAR( walkSeconds / lengths[0], runSeconds / lengths[1], 1e-6F );
    // Unsynced clocks would have each run 0.4 s: walk at 0.4, run at 0.667 — out of step.
    EXPECT_GT( std::abs( 0.4F / lengths[0] - 0.4F / lengths[1] ), 0.2F );

    float wrapped = 0.0F;
    for ( int frame = 0; frame < 60; ++frame ) // 1.0 s of a 0.8 s cycle
        wrapped = G::AdvanceSyncedPhase( wrapped, weights, lengths, 1.0F / 60.0F, true );
    EXPECT_NEAR( wrapped, 0.25F, 1e-3F );
    EXPECT_EQ( G::AdvanceSyncedPhase( 0.9F, weights, lengths, 5.0F, false ), 1.0F ) << "no loop holds the end";
}

TEST( BlendSpace1D, ThePlanRefusesARowThatIsNotStrictlyAscendingOrNamesNoClip )
{
    EXPECT_TRUE( G::BlendSpace1DError( LocomotionRow() ).empty() );
    auto unordered       = LocomotionRow();
    unordered.Samples[2] = G::BlendSample{ "Run", 300.0F };
    EXPECT_NE( G::BlendSpace1DError( unordered ).find( "strictly ascending" ), std::string::npos );
    auto unnamed            = LocomotionRow();
    unnamed.Samples[1].Clip = "";
    EXPECT_NE( G::BlendSpace1DError( unnamed ).find( "names no clip" ), std::string::npos );
    EXPECT_FALSE( G::BlendSpace1DError( G::BlendSpace1DNode{} ).empty() );
    auto negative        = LocomotionRow();
    negative.WeightSpeed = -1.0F;
    EXPECT_FALSE( G::BlendSpace1DError( negative ).empty() );

    G::AnimGraph graph;
    graph.Name       = "BS";
    graph.Parameters = { G::Parameter{ "Speed", static_cast<int>( G::ParamType::Float ), 0.0F } };
    G::PoseNode node;
    node.Name            = "Locomotion";
    node.Kind            = static_cast<int>( G::PoseNodeKind::BlendSpace1D );
    node.ParameterInputs = { G::ParameterPin{ std::string( G::kBlendSpaceAxisPin ), "Speed" } };
    node.BlendSpace      = LocomotionRow();
    graph.Nodes          = { node };
    graph.OutputPose     = "Locomotion";
    EXPECT_TRUE( G::PlanPoseGraph( graph ).IsSuccess() );
    graph.Nodes[0].BlendSpace = unordered;
    EXPECT_FALSE( G::PlanPoseGraph( graph ).IsSuccess() );
    graph.Nodes[0].BlendSpace.reset();
    EXPECT_FALSE( G::PlanPoseGraph( graph ).IsSuccess() ) << "a BlendSpace1D node with no payload";
}
