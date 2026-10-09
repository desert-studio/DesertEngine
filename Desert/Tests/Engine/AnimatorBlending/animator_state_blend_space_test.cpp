// GP2e: a state machine state that plays a Blend Space 1D (Graph::StateBlendSpace) on the Animator's base stage
// (Animator::PlayBlendSpace / CrossFadeBlendSpace) — its pose is every weighted sample's, its clock is the shared
// phase, and the heaviest sample is the clip the rest of the Animator reads (exit time, notifies).
#include <Common/Core/Timestep.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include "../ClipFixture.hpp"

#include <gtest/gtest.h>

#include <array>
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

    /// The pose graph the ECS system sets: a Float Speed (cm/s) and a state machine at the output, whose running
    /// state is the base stage this suite drives directly.
    G::AnimGraph MachineGraph()
    {
        G::AnimGraph graph = G::MakeStateMachineGraph( "Locomotion" );
        graph.Parameters.push_back(
             G::Parameter{ .Name = "Speed", .Type = static_cast<int>( G::ParamType::Float ), .Default = 0.0F } );
        return graph;
    }

    /// The Locomotion state's row: Walk at 0, Run at 300 cm/s over Speed, no weight smoothing.
    G::StateBlendSpace Row()
    {
        G::StateBlendSpace space;
        space.Axis  = "Speed";
        space.Space = G::BlendSpace1DNode{
             .Samples = { G::BlendSample{ "Walk", 0.0F }, G::BlendSample{ "Run", 300.0F } }, .WeightSpeed = 0.0F };
        return space;
    }

    float SpineHeight( const Animator& animator )
    {
        return animator.GetBoneModelMatrix( 1 )[3].y - animator.GetBoneModelMatrix( 0 )[3].y;
    }

    struct Played
    {
        Skeleton      Rig = MakeRig();
        AnimationClip Walk =
             ClipFixture::StaticBoneClip( "Walk", Ticks( 1.0 ), "spine", glm::vec3( 0.0F, 0.0F, 0.0F ) );
        AnimationClip Run =
             ClipFixture::StaticBoneClip( "Run", Ticks( 2.0 ), "spine", glm::vec3( 0.0F, 100.0F, 0.0F ) );
        Animator                            Anim{ Rig };
        std::array<const AnimationClip*, 2> Clips{ &Walk, &Run };

        explicit Played( float speed )
        {
            const auto set = Anim.SetPoseGraph( MachineGraph() );
            EXPECT_TRUE( set ) << ( set ? std::string() : set.GetError() );
            Anim.SetPoseGraphParameter( "Speed", speed );
            Anim.PlayBlendSpace( Row(), Clips );
        }
    };
} // namespace

// Red without SamplePlayback (the base stage sampling only the heaviest clip): halfway on the axis the spine
// would stand at Walk's 0 cm or Run's 100 cm, never their half mix.
TEST( AnimatorStateBlendSpace, AStateBlendSpacePlaysTheWeightedMixOfItsSamples )
{
    Played played( 150.0F );
    played.Anim.Update( Timestep( 0.0F ) );
    EXPECT_NEAR( SpineHeight( played.Anim ), 50.0F, 1e-3F ) << "Speed 150 is the half mix of Walk and Run";

    played.Anim.SetPoseGraphParameter( "Speed", 300.0F );
    played.Anim.Update( Timestep( 0.0F ) );
    EXPECT_NEAR( SpineHeight( played.Anim ), 100.0F, 1e-3F ) << "the axis is read every update, not on entry";
    ASSERT_NE( played.Anim.GetCurrentBlendSpace(), nullptr );
    EXPECT_EQ( played.Anim.GetCurrentBlendSpace()->Axis, "Speed" );
}

// Red if the heaviest sample ran its own clock (Run 0.5 s into 2 s = 0.25) or the state's clock were not the
// heaviest sample's: at Speed 200 Run weighs 2/3, the shared cycle is 1/3 x 1 s + 2/3 x 2 s = 5/3 s, so 0.5 s
// is phase 0.3 — Run at 0.6 s, which is what an exit time reads.
TEST( AnimatorStateBlendSpace, TheHeaviestSampleAtTheSharedPhaseIsTheStatesClock )
{
    Played played( 200.0F );
    played.Anim.Update( Timestep( 0.5F ) );

    const Animator::BlendSpaceRun* run = played.Anim.GetCurrentBlendRun();
    ASSERT_NE( run, nullptr );
    EXPECT_NEAR( run->Phase, 0.3F, 1e-4F );
    EXPECT_EQ( played.Anim.GetCurrentClip(), &played.Run ) << "Run (2/3) is the heaviest sample";
    EXPECT_NEAR( played.Anim.GetDuration(), 2.0F, 1e-4F );
    EXPECT_NEAR( played.Anim.GetCurrentTime() / played.Anim.GetDuration(), run->Phase, 1e-3F )
         << "the exit-time fraction is the shared phase";

    // Past the middle of the axis the heaviest sample, and so the clock, becomes Walk at the same phase.
    played.Anim.SetPoseGraphParameter( "Speed", 60.0F );
    played.Anim.Update( Timestep( 0.0F ) );
    EXPECT_EQ( played.Anim.GetCurrentClip(), &played.Walk );
    EXPECT_NEAR( played.Anim.GetCurrentTime() / played.Anim.GetDuration(), run->Phase, 1e-3F );
}

// Red if a request for the row already playing restarted it: the machine asks for its current state every tick
// (and again once a sample clip finishes loading), so a restart would pin the phase at 0.
TEST( AnimatorStateBlendSpace, RequestingTheRowAlreadyPlayingDoesNotRestartIt )
{
    Played played( 200.0F );
    played.Anim.Update( Timestep( 0.5F ) );
    const float phase = played.Anim.GetCurrentBlendRun()->Phase;
    ASSERT_GT( phase, 0.0F );

    played.Anim.PlayBlendSpace( Row(), played.Clips );
    played.Anim.CrossFadeBlendSpace( Row(), played.Clips, 0.2F );
    ASSERT_NE( played.Anim.GetCurrentBlendRun(), nullptr );
    EXPECT_FLOAT_EQ( played.Anim.GetCurrentBlendRun()->Phase, phase );

    // A sample resolved after the state was entered is taken without a restart.
    played.Anim.PlayBlendSpace( Row(), std::array<const AnimationClip*, 2>{ &played.Walk, nullptr } );
    played.Anim.PlayBlendSpace( Row(), played.Clips );
    EXPECT_FLOAT_EQ( played.Anim.GetCurrentBlendRun()->Phase, phase );

    // A different row is a different state: it starts over.
    G::StateBlendSpace other     = Row();
    other.Space.Samples[1].Value = 600.0F;
    played.Anim.PlayBlendSpace( other, played.Clips );
    EXPECT_FLOAT_EQ( played.Anim.GetCurrentBlendRun()->Phase, 0.0F );
}

// Red without CrossFade's blend-space check: the heaviest sample IS Run, so the old `GetCurrentClip() == &clip`
// early-out would keep the blend space and never fade to the Run clip state.
TEST( AnimatorStateBlendSpace, CrossFadingToTheHeaviestSamplesClipStillFades )
{
    Played played( 300.0F );
    played.Anim.Update( Timestep( 0.1F ) );
    ASSERT_EQ( played.Anim.GetCurrentClip(), &played.Run );

    played.Anim.CrossFade( played.Run, 0.2F );
    EXPECT_EQ( played.Anim.GetCurrentBlendSpace(), nullptr ) << "the Run clip state fades in over the blend space";
    EXPECT_EQ( played.Anim.GetCurrentBlendRun(), nullptr );
}
