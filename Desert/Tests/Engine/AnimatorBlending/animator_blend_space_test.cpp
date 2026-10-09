// GP2d: the Animator plays a Blend Space 1D node (UE FAnimNode_BlendSpacePlayer) — its samples weighted by the
// axis pin's parameter, smoothed at WeightSpeed, every sample at one shared normalized phase.
#include <Common/Core/Timestep.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include "../ClipFixture.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using Common::Timestep;
using Desert::Animation::AnimationClip;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::NearestTick;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::SecondsToFrameTime;
using Desert::Animation::Skeleton;
namespace Animation = Desert::Animation;
namespace G         = Desert::Animation::Graph;

namespace
{
    constexpr size_t kBlendNode = 0;

    BoneInfo MakeBone( const char* name, std::optional<uint32_t> parent, const glm::vec3& at )
    {
        BoneInfo b;
        b.Name               = name;
        b.ParentBoneID       = parent;
        b.LocalBindTransform = glm::translate( glm::mat4( 1.0F ), at );
        return b;
    }

    /// root -> spine; the spine's bind height over the root (30 cm) is neither sample's (0 / 100 cm).
    Skeleton MakeRig()
    {
        std::vector<BoneInfo> bones;
        bones.push_back( MakeBone( "root", std::nullopt, glm::vec3( 0.0F, 90.0F, 0.0F ) ) );
        bones.push_back( MakeBone( "spine", 0U, glm::vec3( 0.0F, 30.0F, 0.0F ) ) );
        Skeleton skeleton( std::move( bones ) );
        skeleton.RecomputeOffsetMatrices();
        return skeleton;
    }

    Animation::FrameNumber Ticks( double seconds )
    {
        return NearestTick( SecondsToFrameTime( seconds, PROJECT_TICK_RATE ) );
    }

    /// A clip of `seconds` holding the spine `height` cm over the root, with a curve "Cycle" rising linearly
    /// from 0 at its start to 1 at its end — the curve reads back the normalized time the clip was sampled at.
    AnimationClip CycleClip( const char* name, float height, double seconds )
    {
        AnimationClip clip =
             ClipFixture::StaticBoneClip( name, Ticks( seconds ), "spine", glm::vec3( 0.0F, height, 0.0F ) );
        Animation::ScalarKey first = ClipFixture::Key( clip.Sequence.Start, 0.0F );
        Animation::ScalarKey last  = ClipFixture::Key( clip.Sequence.End, 1.0F );
        first.Interp               = Animation::KeyInterp::Linear;
        last.Interp                = Animation::KeyInterp::Linear;
        ClipFixture::AddCurve( clip, "Cycle", { first, last } );
        return clip;
    }

    /// Output Pose = Blend Space 1D "Move": Walk at 0, Run at 300 cm/s, X bound to the Float parameter Speed.
    G::AnimGraph BlendSpaceGraph( float weightSpeed )
    {
        G::AnimGraph graph;
        graph.Name = "Locomotion";
        graph.Parameters.push_back(
             G::Parameter{ .Name = "Speed", .Type = static_cast<int>( G::ParamType::Float ), .Default = 0.0F } );
        G::PoseNode move;
        move.Name = "Move";
        move.Kind = static_cast<int>( G::PoseNodeKind::BlendSpace1D );
        move.BlendSpace =
             G::BlendSpace1DNode{ .Samples = { G::BlendSample{ "Walk", 0.0F }, G::BlendSample{ "Run", 300.0F } },
                                  .WeightSpeed = weightSpeed };
        move.ParameterInputs.push_back( G::ParameterPin{ std::string( G::kBlendSpaceAxisPin ), "Speed" } );
        graph.Nodes.push_back( std::move( move ) );
        graph.OutputPose = "Move";
        return graph;
    }

    float SpineHeight( const Animator& animator )
    {
        return animator.GetBoneModelMatrix( 1 )[3].y - animator.GetBoneModelMatrix( 0 )[3].y;
    }

    struct Played
    {
        Skeleton      Rig  = MakeRig();
        AnimationClip Walk = CycleClip( "Walk", 0.0F, 1.0 );
        AnimationClip Run  = CycleClip( "Run", 100.0F, 2.0 );
        Animator      Anim{ Rig };

        explicit Played( float weightSpeed )
        {
            const auto set = Anim.SetPoseGraph( BlendSpaceGraph( weightSpeed ) );
            EXPECT_TRUE( set ) << ( set ? std::string() : set.GetError() );
            Anim.SetPoseGraphBlendSample( kBlendNode, 0, Walk );
            Anim.SetPoseGraphBlendSample( kBlendNode, 1, Run );
        }
    };
} // namespace

// Red without the Animator's blend space run: the node samples no clip and the spine stands at its bind 30 cm.
TEST( AnimatorBlendSpace, BetweenTwoSamplesThePoseIsTheirWeightedMixAndNeitherClip )
{
    Played     played( 0.0F );
    const auto heightAt = [&]( float speed )
    {
        played.Anim.SetPoseGraphParameter( "Speed", speed );
        played.Anim.Update( Timestep( 0.0F ) );
        return SpineHeight( played.Anim );
    };
    EXPECT_NEAR( heightAt( 0.0F ), 0.0F, 1e-3F ) << "Speed at the Walk sample is the Walk pose";
    EXPECT_NEAR( heightAt( 300.0F ), 100.0F, 1e-3F ) << "Speed at the Run sample is the Run pose";
    EXPECT_NEAR( heightAt( 150.0F ), 50.0F, 1e-3F ) << "halfway on the axis is neither clip but their half mix";
    EXPECT_NEAR( heightAt( 75.0F ), 25.0F, 1e-3F ) << "the mix follows the axis linearly";
}

// Red if each sample ran its own clock: after 0.75 s the 1 s Walk would be at 0.75 and the 2 s Run at 0.375.
// Synced, both are at the phase advanced over the weight-averaged 1.5 s cycle: 0.5.
TEST( AnimatorBlendSpace, TheSamplesPlayAtOneSharedNormalizedPhase )
{
    Played played( 0.0F );
    played.Anim.SetPoseGraphParameter( "Speed", 150.0F );
    played.Anim.Update( Timestep( 0.75F ) );

    const Animator::BlendSpaceRun* run = played.Anim.GetPoseGraphBlendSpace( kBlendNode );
    ASSERT_NE( run, nullptr );
    EXPECT_NEAR( run->Phase, 0.5F, 1e-4F );
    const std::optional<float> cycle = played.Anim.GetCurveValue( "Cycle" );
    ASSERT_TRUE( cycle.has_value() ) << "the samples' curves travel with the blend space's pose";
    EXPECT_NEAR( *cycle, 0.5F, 1e-3F ) << "both samples must be read at the one normalized phase";
    EXPECT_EQ( played.Anim.GetPoseGraphBlendSpace( kBlendNode + 1 ), nullptr );
}

// UE TargetWeightInterpolationSpeedPerSec: red if the weights jumped to the axis's target (100 cm at once).
TEST( AnimatorBlendSpace, WeightSpeedMovesTheMixTowardTheAxisOverTime )
{
    Played played( 2.0F );
    played.Anim.SetPoseGraphParameter( "Speed", 0.0F );
    played.Anim.Update( Timestep( 0.0F ) ); // the first update takes the target: all Walk
    EXPECT_NEAR( SpineHeight( played.Anim ), 0.0F, 1e-3F );

    played.Anim.SetPoseGraphParameter( "Speed", 300.0F );
    played.Anim.Update( Timestep( 0.25F ) ); // 2 per second x 0.25 s: half the way
    const Animator::BlendSpaceRun* run = played.Anim.GetPoseGraphBlendSpace( kBlendNode );
    ASSERT_NE( run, nullptr );
    ASSERT_EQ( run->Weights.size(), 2U );
    EXPECT_NEAR( run->Weights[0], 0.5F, 1e-4F );
    EXPECT_NEAR( run->Weights[1], 0.5F, 1e-4F );
    EXPECT_NEAR( SpineHeight( played.Anim ), 50.0F, 1e-3F );

    played.Anim.Update( Timestep( 0.5F ) );
    EXPECT_NEAR( SpineHeight( played.Anim ), 100.0F, 1e-3F ) << "the weights arrive and stop at the target";
}
