// ANIM-FIX10: the pose graph's skeletal control nodes (UE AnimGraph's Two Bone IK and Look At). The solve is
// the bone-control stage's own (BoneControlContract above pins it); this file pins what the NODE adds: it
// takes its input pose from its wire, reaches the authored goal, honours Alpha (its own and a bound pin's)
// and reads a target in a named bone's space against the input pose.
//
// MUTATIONS: BoneControlNodes.cpp ApplyTwoBoneIKNode passes `1.0F` to ApplyBoneOverrides instead of `alpha`
// -> AHalfAlphaNodeStopsHalfwayAndZeroIsBitExact reds; PoseGraphInstance.cpp `pose = in;` after the solve
// unconditionally -> ATwoBoneIKNodeLandsTheEndOfTheChainOnItsGoal reds.

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Graph/PoseGraphInstance.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace
{
    namespace A = Desert::Animation;
    namespace G = Desert::Animation::Graph;

    A::Skeleton MakeArm()
    {
        const auto place = []( const glm::vec3& t, float degrees, const glm::vec3& axis )
        {
            return glm::translate( glm::mat4( 1.0F ), t ) *
                   glm::rotate( glm::mat4( 1.0F ), glm::radians( degrees ), axis );
        };
        std::vector<A::BoneInfo> bones( 4 );
        bones[0].Name               = "Spine";
        bones[0].LocalBindTransform = place( { 0.0F, 100.0F, 0.0F }, 15.0F, { 0.0F, 0.0F, 1.0F } );
        bones[1].Name               = "Shoulder";
        bones[1].ParentBoneID       = 0U;
        bones[1].LocalBindTransform = place( { 10.0F, 20.0F, 0.0F }, -10.0F, { 1.0F, 0.0F, 0.0F } );
        bones[2].Name               = "Elbow";
        bones[2].ParentBoneID       = 1U;
        bones[2].LocalBindTransform = place( { 0.0F, -40.0F, 3.0F }, 5.0F, { 0.0F, 1.0F, 0.0F } );
        bones[3].Name               = "Hand";
        bones[3].ParentBoneID       = 2U;
        bones[3].LocalBindTransform = place( { 0.0F, -30.0F, 0.0F }, -8.0F, { 0.0F, 0.0F, 1.0F } );
        A::Skeleton rig( std::move( bones ) );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    constexpr uint32_t kHand = 3;

    /// Source -> `control` -> Output Pose; the source plays the bind pose.
    G::AnimGraph ControlGraph( G::PoseNode control )
    {
        G::AnimGraph graph;
        graph.Name = "Controls";
        G::PoseNode source;
        source.Name        = "Source";
        source.Kind        = static_cast<int>( G::PoseNodeKind::SequencePlayer );
        source.Sequence    = G::SequencePlayerNode{ "Bind", true };
        control.Name       = "Control";
        control.PoseInputs = { "Source" };
        graph.Nodes        = { source, control };
        graph.OutputPose   = "Control";
        return graph;
    }

    G::PoseNode IKNode( const glm::vec3& goal, float alpha )
    {
        G::PoseNode node;
        node.Kind                     = static_cast<int>( G::PoseNodeKind::TwoBoneIK );
        node.TwoBoneIK                = G::TwoBoneIKNode{};
        node.TwoBoneIK->EndBone       = "Hand";
        node.TwoBoneIK->Goal.Position = { goal.x, goal.y, goal.z };
        node.TwoBoneIK->Alpha         = alpha;
        return node;
    }

    A::LocalPose Evaluate( const A::Skeleton& rig, const G::AnimGraph& graph, float boundAlpha = -1.0F )
    {
        G::PoseGraphInstance instance;
        const auto           bound = instance.Bind( graph, rig );
        EXPECT_TRUE( bound.IsSuccess() ) << bound.GetError();
        G::PoseGraphSources sources;
        sources.Sample = [&rig]( size_t, G::GraphPose& out )
        {
            for ( size_t b = 0; b < rig.GetBones().size(); ++b )
                out.Pose[b] = A::BoneTransform::FromMatrix( rig.GetBones()[b].LocalBindTransform ).GetValue();
        };
        sources.Parameter = [boundAlpha]( const std::string& ) { return boundAlpha; };
        G::GraphPose out;
        instance.Evaluate( sources, rig, out );
        return out.Pose;
    }

    glm::vec3 ComponentPosition( const A::Skeleton& rig, const A::LocalPose& pose, uint32_t bone )
    {
        A::ComponentPose component( rig, pose );
        return glm::vec3( component.Get( bone )[3] );
    }
} // namespace

TEST( BoneControlNodes, ATwoBoneIKNodeLandsTheEndOfTheChainOnItsGoal )
{
    const A::Skeleton  rig = MakeArm();
    const glm::vec3    goal( 30.0F, 70.0F, 20.0F );
    const A::LocalPose pose = Evaluate( rig, ControlGraph( IKNode( goal, 1.0F ) ) );
    EXPECT_LT( glm::length( ComponentPosition( rig, pose, kHand ) - goal ), 0.01F );
}

TEST( BoneControlNodes, AHalfAlphaNodeStopsHalfwayAndZeroIsBitExact )
{
    const A::Skeleton  rig = MakeArm();
    const glm::vec3    goal( 30.0F, 70.0F, 20.0F );
    const A::LocalPose bind = Evaluate( rig, ControlGraph( IKNode( goal, 0.0F ) ) );
    const A::LocalPose half = Evaluate( rig, ControlGraph( IKNode( goal, 0.5F ) ) );
    const A::LocalPose full = Evaluate( rig, ControlGraph( IKNode( goal, 1.0F ) ) );

    for ( size_t b = 0; b < rig.GetBones().size(); ++b )
    {
        const A::BoneTransform expected =
             A::BoneTransform::FromMatrix( rig.GetBones()[b].LocalBindTransform ).GetValue();
        EXPECT_EQ( bind[b].Translation, expected.Translation ) << b;
        EXPECT_EQ( bind[b].Rotation, expected.Rotation ) << b;
    }
    const float toGoalHalf = glm::length( ComponentPosition( rig, half, kHand ) - goal );
    const float toGoalBind = glm::length( ComponentPosition( rig, bind, kHand ) - goal );
    EXPECT_GT( toGoalHalf, 0.5F ) << "alpha 0.5 reached the goal: the node ignores its Alpha";
    EXPECT_LT( toGoalHalf, toGoalBind );
    EXPECT_LT( glm::length( ComponentPosition( rig, full, kHand ) - goal ), 0.01F );
}

TEST( BoneControlNodes, ABoundAlphaPinOverridesTheNodesOwnAlpha )
{
    const A::Skeleton rig = MakeArm();
    const glm::vec3   goal( 30.0F, 70.0F, 20.0F );
    G::AnimGraph      graph = ControlGraph( IKNode( goal, 1.0F ) );
    graph.Parameters.push_back( G::Parameter{} );
    graph.Parameters.back().Name = "Reach";
    graph.Parameters.back().Type = static_cast<int>( G::ParamType::Float );
    graph.Nodes[1].ParameterInputs.push_back( { std::string( G::kBoneControlAlphaPin ), "Reach" } );
    const A::LocalPose off = Evaluate( rig, graph, 0.0F );
    EXPECT_GT( glm::length( ComponentPosition( rig, off, kHand ) - goal ), 1.0F );
}

TEST( BoneControlNodes, AGoalInABonesSpaceRidesWithThatBone )
{
    const A::Skeleton rig   = MakeArm();
    G::PoseNode       node  = IKNode( glm::vec3( 0.0F ), 1.0F );
    node.TwoBoneIK->Goal    = G::BoneControlTarget{ { 20.0F, -30.0F, 10.0F }, "Spine" };
    const A::LocalPose pose = Evaluate( rig, ControlGraph( node ) );
    const glm::vec3    goal =
         glm::vec3( rig.GetBones()[0].LocalBindTransform * glm::vec4( 20.0F, -30.0F, 10.0F, 1.0F ) );
    EXPECT_LT( glm::length( ComponentPosition( rig, pose, kHand ) - goal ), 0.01F );
}

TEST( BoneControlNodes, ALookAtNodeTurnsTheAimAxisOntoTheTarget )
{
    const A::Skeleton rig = MakeArm();
    G::PoseNode       node;
    node.Kind                    = static_cast<int>( G::PoseNodeKind::LookAt );
    node.LookAt                  = G::LookAtNode{};
    node.LookAt->Bone            = "Elbow";
    node.LookAt->Target.Position = { 60.0F, 150.0F, -40.0F };
    node.LookAt->AimAxis         = { 0.0F, -1.0F, 0.0F };
    const A::LocalPose pose      = Evaluate( rig, ControlGraph( node ) );

    A::ComponentPose component( rig, pose );
    const glm::mat4  elbow = component.Get( 2 );
    const glm::vec3  aim   = glm::normalize( glm::vec3( elbow * glm::vec4( 0.0F, -1.0F, 0.0F, 0.0F ) ) );
    const glm::vec3  to    = glm::normalize( glm::vec3( 60.0F, 150.0F, -40.0F ) - glm::vec3( elbow[3] ) );
    EXPECT_GT( glm::dot( aim, to ), 0.9999F );
}

TEST( BoneControlNodes, ANodeWithoutItsSetupOrWithAnUnknownBoneIsRefusedOrPassesThrough )
{
    const A::Skeleton rig = MakeArm();
    G::PoseNode       bare;
    bare.Kind = static_cast<int>( G::PoseNodeKind::TwoBoneIK );
    G::PoseGraphInstance instance;
    EXPECT_FALSE( instance.Bind( ControlGraph( bare ), rig ).IsSuccess() );

    G::PoseNode unknown          = IKNode( glm::vec3( 30.0F, 70.0F, 20.0F ), 1.0F );
    unknown.TwoBoneIK->EndBone   = "Tentacle";
    const A::LocalPose pose      = Evaluate( rig, ControlGraph( unknown ) );
    const A::LocalPose untouched = Evaluate( rig, ControlGraph( IKNode( glm::vec3( 0.0F ), 0.0F ) ) );
    for ( size_t b = 0; b < rig.GetBones().size(); ++b )
        EXPECT_EQ( pose[b].Rotation, untouched[b].Rotation ) << b;
}
