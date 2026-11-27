// GP2d: the Blend Space 1D node (UE UBlendSpace1D on a Blend Space Player) — its weights on the axis, the weight
// smoothing (TargetWeightInterpolationSpeedPerSec), the phase sync of its samples and the plan's refusals; and the
// 2D space's triangulated weights (UE UBlendSpace, bInterpolateUsingGrid off).
#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace G = Desert::Animation::Graph;

namespace
{
    // UE-like locomotion row in cm/s: Idle at 0, Walk at 300, Run at 600, on an axis 0..600.
    G::BlendSpaceSetup LocomotionRow()
    {
        G::BlendSpaceSetup setup;
        setup.Axes    = { G::BlendSpaceAxis{ .Min = 0.0F, .Max = 600.0F } };
        setup.Samples = { G::BlendSample{ .Clip = "Idle", .X = 0.0F },
                          G::BlendSample{ .Clip = "Walk", .X = 300.0F },
                          G::BlendSample{ .Clip = "Run", .X = 600.0F } };
        return setup;
    }

    std::array<float, 3> WeightsAt( float x )
    {
        const G::BlendSpaceSetup         row = LocomotionRow();
        const G::BlendSpaceTriangulation tri = G::TriangulateBlendSpace( row );
        std::vector<float>               w;
        const std::array<float, 1>       axis{ x };
        G::BlendSpaceTargetWeights( row, tri, axis, w );
        return { w[0], w[1], w[2] };
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
    std::vector<float>       weights{ 1.0F, 0.0F, 0.0F };
    const std::vector<float> target{ 0.0F, 0.0F, 1.0F };
    G::InterpolateBlendWeights( weights, target, 2.0F, false, 0.1F ); // 0.2 of weight in this step
    EXPECT_NEAR( weights[0], 0.8F / 1.0F, 1e-5F );
    EXPECT_NEAR( weights[2], 0.2F / 1.0F, 1e-5F );
    EXPECT_NEAR( weights[0] + weights[1] + weights[2], 1.0F, 1e-6F );
    for ( int i = 0; i < 10; ++i )
        G::InterpolateBlendWeights( weights, target, 2.0F, false, 0.1F );
    EXPECT_EQ( weights, target ) << "a second at 2/s reaches the target";

    std::vector<float> snapped{ 1.0F, 0.0F, 0.0F };
    G::InterpolateBlendWeights( snapped, target, 0.0F, false, 0.016F );
    EXPECT_EQ( snapped, target ) << "WeightSpeed 0 is no smoothing";
}

// Two cycles of different length blended half and half: the shared phase is what both samples play at, so the
// normalized times stay equal (the feet stay in step), and the blend's cycle lasts the weight-averaged length.
TEST( BlendSpace1D, PhaseSyncKeepsTwoCyclesOfDifferentLengthAtOneNormalizedTime )
{
    const std::array<float, 2> weights{ 0.5F, 0.5F };
    const std::array<float, 2> lengths{ 1.0F, 0.6F }; // walk 1.0 s, run 0.6 s -> blended cycle 0.8 s
    const std::array<G::BlendSample, 2> samples{ G::BlendSample{ .Clip = "Walk" },
                                                 G::BlendSample{ .Clip = "Run", .X = 1.0F } };
    float                      phase = 0.0F;
    for ( int frame = 0; frame < 24; ++frame )
        phase = G::AdvanceSyncedPhase( phase, weights, lengths, samples, 1.0F, true, 1.0F / 60.0F );
    EXPECT_NEAR( phase, 0.4F / 0.8F, 1e-4F );
    // Each sample's own normalized time is the phase: its seconds over its length agree.
    const float walkSeconds = phase * lengths[0];
    const float runSeconds  = phase * lengths[1];
    EXPECT_NEAR( walkSeconds / lengths[0], runSeconds / lengths[1], 1e-6F );
    // Unsynced clocks would have each run 0.4 s: walk at 0.4, run at 0.667 — out of step.
    EXPECT_GT( std::abs( 0.4F / lengths[0] - 0.4F / lengths[1] ), 0.2F );

    float wrapped = 0.0F;
    for ( int frame = 0; frame < 60; ++frame ) // 1.0 s of a 0.8 s cycle
        wrapped = G::AdvanceSyncedPhase( wrapped, weights, lengths, samples, 1.0F, true, 1.0F / 60.0F );
    EXPECT_NEAR( wrapped, 0.25F, 1e-3F );
    EXPECT_EQ( G::AdvanceSyncedPhase( 0.9F, weights, lengths, samples, 1.0F, false, 5.0F ), 1.0F )
         << "no loop holds the end";
}

TEST( BlendSpace1D, ThePlanRefusesTwoSamplesAtOnePositionASampleOutsideTheAxisOrNoClip )
{
    EXPECT_TRUE( G::BlendSpaceError( LocomotionRow(), 1 ).empty() );
    auto doubled         = LocomotionRow();
    doubled.Samples[2].X = 300.0F;
    EXPECT_NE( G::BlendSpaceError( doubled, 1 ).find( "at one position" ), std::string::npos );
    auto outside         = LocomotionRow();
    outside.Samples[2].X = 900.0F;
    EXPECT_NE( G::BlendSpaceError( outside, 1 ).find( "outside the axis range" ), std::string::npos );
    auto unnamed            = LocomotionRow();
    unnamed.Samples[1].Clip = "";
    EXPECT_NE( G::BlendSpaceError( unnamed, 1 ).find( "names no clip" ), std::string::npos );
    EXPECT_FALSE( G::BlendSpaceError( G::BlendSpaceSetup{}, 1 ).empty() );
    EXPECT_FALSE( G::BlendSpaceError( LocomotionRow(), 2 ).empty() ) << "one axis is not a 2D space";
    auto negative        = LocomotionRow();
    negative.WeightSpeed = -1.0F;
    EXPECT_FALSE( G::BlendSpaceError( negative, 1 ).empty() );

    G::AnimGraph graph;
    graph.Name       = "BS";
    graph.Parameters = { G::Parameter{ "Speed", static_cast<int>( G::ParamType::Float ), 0.0F } };
    G::PoseNode node;
    node.Name            = "Locomotion";
    node.Kind            = static_cast<int>( G::PoseNodeKind::BlendSpace1D );
    node.ParameterInputs = { G::ParameterPin{ std::string( G::kBlendSpaceXPin ), "Speed" } };
    node.BlendSpace      = LocomotionRow();
    graph.Nodes          = { node };
    graph.OutputPose     = "Locomotion";
    EXPECT_TRUE( G::PlanPoseGraph( graph ).IsSuccess() );
    graph.Nodes[0].BlendSpace = doubled;
    EXPECT_FALSE( G::PlanPoseGraph( graph ).IsSuccess() );
    graph.Nodes[0].BlendSpace.reset();
    EXPECT_FALSE( G::PlanPoseGraph( graph ).IsSuccess() ) << "a BlendSpace1D node with no payload";
}

// Speed x crouch: a sample's own position gives it the whole weight, every point's weights sum to one, and a
// point outside the samples' hull (but inside the axis box) still weighs only hull samples.
TEST( BlendSpace2D, TriangulatedWeightsAreWholeAtASampleAndSumToOneEverywhere )
{
    G::BlendSpaceSetup space;
    space.Axes    = { G::BlendSpaceAxis{ .Min = 0.0F, .Max = 600.0F },
                      G::BlendSpaceAxis{ .Min = 0.0F, .Max = 1.0F } };
    space.Samples = { G::BlendSample{ .Clip = "Idle", .X = 0.0F, .Y = 0.0F },
                      G::BlendSample{ .Clip = "Walk", .X = 150.0F, .Y = 0.0F },
                      G::BlendSample{ .Clip = "Jog", .X = 600.0F, .Y = 0.0F },
                      G::BlendSample{ .Clip = "CrouchIdle", .X = 0.0F, .Y = 1.0F },
                      G::BlendSample{ .Clip = "CrouchWalk", .X = 150.0F, .Y = 1.0F } };
    ASSERT_TRUE( G::BlendSpaceError( space, 2 ).empty() );
    const G::BlendSpaceTriangulation tri = G::TriangulateBlendSpace( space );
    EXPECT_FALSE( tri.Triangles.empty() );
    std::vector<float> w;
    for ( size_t s = 0; s < space.Samples.size(); ++s )
    {
        const std::array<float, 2> at{ space.Samples[s].X, space.Samples[s].Y };
        G::BlendSpaceTargetWeights( space, tri, at, w );
        EXPECT_NEAR( w[s], 1.0F, 1e-4F ) << "sample " << s;
    }
    for ( float x = 0.0F; x <= 600.0F; x += 75.0F )
        for ( float y = 0.0F; y <= 1.0F; y += 0.25F )
        {
            const std::array<float, 2> at{ x, y };
            G::BlendSpaceTargetWeights( space, tri, at, w );
            EXPECT_NEAR( std::accumulate( w.begin(), w.end(), 0.0F ), 1.0F, 1e-4F ) << x << ", " << y;
        }
}
