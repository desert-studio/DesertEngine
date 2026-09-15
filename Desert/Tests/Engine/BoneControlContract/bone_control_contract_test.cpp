// THE CONTRACT, NOT THE SOLVER. The maths has its own suite (`TwoBoneIKSolver`) and needs no rig; this one
// is about the four promises `BoneControl.hpp` makes, each of which is a property a FUTURE solver inherits
// for free and therefore has to be pinned once:
//
//   1. SPARSENESS IS A CONTRACT, NOT AN OPTIMISATION. A control that names three bones must leave the other
//      five bit-for-bit as it found them — including the ones that are DESCENDANTS of the chain, whose
//      component transform legitimately moves while their local transform must not.
//   2. THE BLEND IS IN LOCAL SPACE, and it is a different pose from the component-space blend. The number is
//      measured here rather than asserted as "different", because "different" is what a defect also says.
//   3. ALPHA 0 IS BIT-EXACT PASS-THROUGH. Not "close": the same bytes as a pipeline with no control at all.
//      A frame diff can then be required to be exactly zero.
//   4. ORDERING IS ENFORCED, AND SO IS "NO EMPTY SUCCESSFUL ANSWER". Both are refusals with names in them.
//
// The rig is eight bones on purpose. Every rig in this repository's tests before А2 was one or two, and a
// one-bone rig cannot tell "wrote three bones" from "wrote the whole pose" — the two produce the same
// bytes. Five bones outside the chain is what makes promise 1 observable at all.

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/BoneControl.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TwoBoneIKControl.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using Desert::Animation::Animator;
using Desert::Animation::BoneControl;
using Desert::Animation::BoneControlKind;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneOverride;
using Desert::Animation::BoneTransform;
using Desert::Animation::ComponentPose;
using Desert::Animation::LocalPose;
using Desert::Animation::PoseStage;
using Desert::Animation::Skeleton;
using Desert::Animation::TwoBoneIKControl;

namespace
{
    // 1 world unit = 1 cm. An eight-bone rig with one two-bone arm in it:
    //
    //   0 Spine (root) -> 1 Shoulder -> 2 Elbow -> 3 Hand -> 4 Finger      the chain is 1/2/3; 4 rides along
    //                  -> 5 Tail -> 6 TailTip                             a sibling branch, must not move
    //   7 Prop (a second root)                                            untouched in every space
    //
    // Every bind transform carries a rotation as well as a translation, for the reason А2's rig exists: a
    // rig whose bind is pure translation cannot tell a bone's own space from skinning space.
    Skeleton MakeArmRig()
    {
        const auto place = []( const glm::vec3& translation, float degrees, const glm::vec3& axis )
        {
            return glm::translate( glm::mat4( 1.0F ), translation ) *
                   glm::rotate( glm::mat4( 1.0F ), glm::radians( degrees ), axis );
        };

        std::vector<BoneInfo> bones( 8 );

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

        bones[4].Name               = "Finger";
        bones[4].ParentBoneID       = 3U;
        bones[4].LocalBindTransform = place( { 0.0F, -5.0F, 0.0F }, 3.0F, { 1.0F, 0.0F, 0.0F } );

        bones[5].Name               = "Tail";
        bones[5].ParentBoneID       = 0U;
        bones[5].LocalBindTransform = place( { -20.0F, -10.0F, 0.0F }, 25.0F, { 0.0F, 1.0F, 0.0F } );

        bones[6].Name               = "TailTip";
        bones[6].ParentBoneID       = 5U;
        bones[6].LocalBindTransform = place( { 0.0F, -10.0F, 0.0F }, -12.0F, { 0.0F, 0.0F, 1.0F } );

        bones[7].Name               = "Prop";
        bones[7].LocalBindTransform = place( { 200.0F, 0.0F, 0.0F }, 40.0F, { 0.0F, 1.0F, 0.0F } );

        Skeleton rig( std::move( bones ) );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    constexpr uint32_t kShoulder = 1;
    constexpr uint32_t kElbow    = 2;
    constexpr uint32_t kHand     = 3;

    bool SameBytes( const BoneTransform& a, const BoneTransform& b )
    {
        return a.Translation == b.Translation && a.Rotation == b.Rotation && a.Scale == b.Scale;
    }

    std::unique_ptr<TwoBoneIKControl> MakeArmIK( const Skeleton& rig, const glm::vec3& goal, float alpha )
    {
        auto control = std::make_unique<TwoBoneIKControl>();
        control->SetEndBone( "Hand" );
        control->SetGoal( goal );
        // Well off the goal line and on the +Z side, so the bend plane is chosen by the pole and not by a
        // fallback — which is a different code path with a different meaning (see the solver suite).
        control->SetPoleTarget( glm::vec3( 10.0F, 60.0F, 120.0F ) );
        control->SetAlpha( alpha );
        static_cast<void>( control->Resolve( rig ) );
        return control;
    }

    /// The pose an Animator produces with the given control (or none), after one Update with no clip: the
    /// source stage falls back to the bind pose bone by bone, which is exactly "a rig standing still".
    LocalPose PoseWith( const Skeleton& rig, std::unique_ptr<BoneControl> control )
    {
        Animator animator( rig );
        if ( control )
        {
            animator.AddControl( std::move( control ) );
        }
        animator.Update( Common::Timestep( 1.0F / 60.0F ) );
        return animator.GetLocalPose();
    }

    /// A control that reports a successful solve and writes nothing — the shape the contract forbids.
    class SilentControl final : public BoneControl
    {
    public:
        [[nodiscard]] BoneControlKind GetKind() const override
        {
            return BoneControlKind::TwoBoneIK;
        }
        [[nodiscard]] Common::BoolResultStr Resolve( const Skeleton& ) override
        {
            return Common::MakeSuccess( true );
        }

    protected:
        [[nodiscard]] Common::BoolResultStr Solve( const Skeleton&, ComponentPose&,
                                                   std::vector<BoneOverride>& ) override
        {
            return Common::MakeSuccess( true );
        }
    };
} // namespace

// ---------------------------------------------------------------- promise 1: sparseness

TEST( BoneControlContract, AControlWritesONLYTheBonesItNamed )
{
    const Skeleton  rig    = MakeArmRig();
    const LocalPose plain  = PoseWith( rig, nullptr );
    const LocalPose solved = PoseWith( rig, MakeArmIK( rig, glm::vec3( 30.0F, 70.0F, 20.0F ), 1.0F ) );

    ASSERT_EQ( plain.Size(), 8U );
    ASSERT_EQ( solved.Size(), 8U );

    for ( uint32_t bone = 0; bone < plain.Size(); ++bone )
    {
        const bool inChain = bone == kShoulder || bone == kElbow || bone == kHand;
        if ( inChain )
        {
            EXPECT_FALSE( SameBytes( plain[bone], solved[bone] ) )
                 << "bone " << bone << " ('" << rig.GetBones()[bone].Name
                 << "') is in the chain and the solve left it untouched — the control did nothing.";
        }
        else
        {
            // BIT-FOR-BIT, not "close". A sparse contract that tolerates a ULP of drift on untouched bones
            // is one that has already converted every bone and thrown the result away.
            EXPECT_TRUE( SameBytes( plain[bone], solved[bone] ) )
                 << "bone " << bone << " ('" << rig.GetBones()[bone].Name
                 << "') is outside the chain and the solve changed its LOCAL transform.";
        }
    }
}

TEST( BoneControlContract, AChildOfTheChainRidesAlongWithoutItsOwnTransformChanging )
{
    const Skeleton rig = MakeArmRig();

    Animator plain( rig );
    plain.Update( Common::Timestep( 1.0F / 60.0F ) );
    const glm::mat4 fingerBefore = plain.GetBoneModelMatrix( 4 );
    const glm::mat4 tailBefore   = plain.GetBoneModelMatrix( 6 );

    Animator solved( rig );
    solved.AddControl( MakeArmIK( rig, glm::vec3( 30.0F, 70.0F, 20.0F ), 1.0F ) );
    solved.Update( Common::Timestep( 1.0F / 60.0F ) );

    // THE RELATION, NOT THE TWO SIDES. "Finger's local is unchanged" and "Finger moved" are both required,
    // and either alone is satisfied by a defect: a control that wrote the whole pose would keep it in place,
    // and one that forgot to invalidate the component cache would move it without moving the hand.
    EXPECT_TRUE( SameBytes( plain.GetLocalPose()[4], solved.GetLocalPose()[4] ) );
    const float fingerTravel =
         glm::length( glm::vec3( solved.GetBoneModelMatrix( 4 )[3] ) - glm::vec3( fingerBefore[3] ) );
    EXPECT_GT( fingerTravel, 1.0F ) << "the chain moved and its child did not follow it";

    // The sibling branch is the negative control of the same statement.
    const float tailTravel =
         glm::length( glm::vec3( solved.GetBoneModelMatrix( 6 )[3] ) - glm::vec3( tailBefore[3] ) );
    EXPECT_LT( tailTravel, 1e-4F ) << "a bone on a different branch moved by " << tailTravel << " cm";
}

// ---------------------------------------------------------------- promise 2: the blend is LOCAL

TEST( BoneControlContract, HalfAppliedIsTheLocalSpaceBlendAndNotTheComponentSpaceOne )
{
    const Skeleton  rig = MakeArmRig();
    const glm::vec3 goal( 30.0F, 70.0F, 20.0F );

    const LocalPose plain = PoseWith( rig, nullptr );
    const LocalPose full  = PoseWith( rig, MakeArmIK( rig, goal, 1.0F ) );
    const LocalPose half  = PoseWith( rig, MakeArmIK( rig, goal, 0.5F ) );

    // What "blended in local space" MEANS, spelled out rather than asserted by name.
    for ( const uint32_t bone : { kShoulder, kElbow, kHand } )
    {
        const BoneTransform expected = Desert::Animation::Blend( plain[bone], full[bone], 0.5F );
        EXPECT_NEAR( glm::length( half[bone].Translation - expected.Translation ), 0.0F, 1e-3F );
        EXPECT_NEAR( glm::length( half[bone].Rotation - expected.Rotation ), 0.0F, 1e-4F );
    }

    // AND IT IS A DIFFERENT POSE from the component-space blend, measured in centimetres on the hand. If
    // this ever comes back as zero, the two spaces have stopped disagreeing and this test proves nothing —
    // which is the failure mode А2's rig was built to make impossible to miss.
    Animator plainAnimator( rig );
    plainAnimator.Update( Common::Timestep( 1.0F / 60.0F ) );
    Animator fullAnimator( rig );
    fullAnimator.AddControl( MakeArmIK( rig, goal, 1.0F ) );
    fullAnimator.Update( Common::Timestep( 1.0F / 60.0F ) );
    Animator halfAnimator( rig );
    halfAnimator.AddControl( MakeArmIK( rig, goal, 0.5F ) );
    halfAnimator.Update( Common::Timestep( 1.0F / 60.0F ) );

    const glm::vec3 componentBlended = 0.5F * glm::vec3( plainAnimator.GetBoneModelMatrix( kHand )[3] ) +
                                       0.5F * glm::vec3( fullAnimator.GetBoneModelMatrix( kHand )[3] );
    const float disagreement =
         glm::length( glm::vec3( halfAnimator.GetBoneModelMatrix( kHand )[3] ) - componentBlended );
    // 2.29 cm on this rig, measured. The floor is 0.1 rather than the measurement so the test pins the
    // PROPERTY (the two spaces are distinguishable here) and not a number that a legitimate change to the
    // fixture would move.
    EXPECT_GT( disagreement, 0.1F ) << "local-space and component-space blending agree to " << disagreement
                                    << " cm on this rig, so this rig cannot tell them apart";
}

// ---------------------------------------------------------------- promise 3: the alpha endpoints

TEST( BoneControlContract, AlphaZeroIsBitExactPassThrough )
{
    const Skeleton  rig  = MakeArmRig();
    const LocalPose none = PoseWith( rig, nullptr );
    const LocalPose off  = PoseWith( rig, MakeArmIK( rig, glm::vec3( 30.0F, 70.0F, 20.0F ), 0.0F ) );

    for ( uint32_t bone = 0; bone < none.Size(); ++bone )
    {
        EXPECT_TRUE( SameBytes( none[bone], off[bone] ) )
             << "bone " << bone
             << " differs with the control at alpha 0. A slerp at alpha 0 is not the "
                "identity on every quaternion, which is exactly why this is short-circuited.";
    }
}

TEST( BoneControlContract, AlphaOneLandsTheEndBoneOnTheGoal )
{
    const Skeleton  rig = MakeArmRig();
    const glm::vec3 goal( 30.0F, 70.0F, 20.0F ); // within reach of the 40.1 / 30 cm chain from the shoulder

    Animator animator( rig );
    animator.AddControl( MakeArmIK( rig, goal, 1.0F ) );
    animator.Update( Common::Timestep( 1.0F / 60.0F ) );

    const glm::vec3 hand = glm::vec3( animator.GetBoneModelMatrix( kHand )[3] );
    EXPECT_NEAR( glm::length( hand - goal ), 0.0F, 0.05F )
         << "the hand ended " << glm::length( hand - goal ) << " cm from the goal";
}

// ---------------------------------------------------------------- promise 4: the refusals

TEST( BoneControlContract, OverridesOutOfOrderAreRefusedByName )
{
    const Skeleton rig  = MakeArmRig();
    auto           bind = LocalPose::FromBindPose( rig );
    ASSERT_TRUE( bind.IsSuccess() );
    LocalPose     pose = bind.GetValue();
    ComponentPose view( rig, pose );

    // Hand before Shoulder: a child converted against a parent that has not been blended yet.
    std::vector<BoneOverride> backwards;
    backwards.push_back( { kHand, pose[kHand] } );
    backwards.push_back( { kShoulder, pose[kShoulder] } );

    std::vector<BoneTransform> scratch;
    const auto                 refused = ApplyBoneOverrides( rig, pose, view, backwards, 1.0F, scratch );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Shoulder" ), std::string::npos )
         << "the refusal must name the bone that is out of place: " << refused.GetError();
}

TEST( BoneControlContract, AnOverrideForABoneTheRigDoesNotHaveIsRefused )
{
    const Skeleton rig  = MakeArmRig();
    auto           bind = LocalPose::FromBindPose( rig );
    ASSERT_TRUE( bind.IsSuccess() );
    LocalPose     pose = bind.GetValue();
    ComponentPose view( rig, pose );

    std::vector<BoneOverride> past;
    past.push_back( { 99U, pose[0] } );

    std::vector<BoneTransform> scratch;
    const auto                 refused = ApplyBoneOverrides( rig, pose, view, past, 1.0F, scratch );
    EXPECT_FALSE( refused.IsSuccess() );
}

TEST( BoneControlContract, ASolveThatWritesNothingIsARefusalRatherThanASuccess )
{
    const Skeleton rig  = MakeArmRig();
    auto           bind = LocalPose::FromBindPose( rig );
    ASSERT_TRUE( bind.IsSuccess() );
    LocalPose     pose = bind.GetValue();
    ComponentPose view( rig, pose );

    SilentControl silent;
    const auto    refused = silent.Evaluate( rig, pose, view );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_FALSE( silent.GetLastError().empty() );
}

TEST( BoneControlContract, TheChainIsDerivedAndEachWayItCanFailHasItsOwnSentence )
{
    const Skeleton rig = MakeArmRig();

    TwoBoneIKControl unnamed;
    const auto       noName = unnamed.Resolve( rig );
    ASSERT_FALSE( noName.IsSuccess() );

    TwoBoneIKControl unknown;
    unknown.SetEndBone( "NoSuchBone" );
    const auto notOnRig = unknown.Resolve( rig );
    ASSERT_FALSE( notOnRig.IsSuccess() );
    EXPECT_NE( notOnRig.GetError().find( "NoSuchBone" ), std::string::npos );

    TwoBoneIKControl onARoot;
    onARoot.SetEndBone( "Spine" );
    const auto rootFails = onARoot.Resolve( rig );
    ASSERT_FALSE( rootFails.IsSuccess() );

    TwoBoneIKControl tooShort;
    tooShort.SetEndBone( "Shoulder" ); // parent Spine is a root: one segment, not two
    const auto shortFails = tooShort.Resolve( rig );
    ASSERT_FALSE( shortFails.IsSuccess() );

    // FOUR DISTINCT SENTENCES, because they are four distinct things for an artist to fix. Collapsing them
    // into one "invalid chain" is the failure this project keeps finding: a refusal nobody can act on.
    const std::vector<std::string> messages{ noName.GetError(), notOnRig.GetError(), rootFails.GetError(),
                                             shortFails.GetError() };
    for ( size_t a = 0; a < messages.size(); ++a )
    {
        for ( size_t b = a + 1; b < messages.size(); ++b )
        {
            EXPECT_NE( messages[a], messages[b] ) << "two different chain failures produce the same message";
        }
    }

    TwoBoneIKControl good;
    good.SetEndBone( "Hand" );
    EXPECT_TRUE( good.Resolve( rig ).IsSuccess() );
    EXPECT_EQ( good.GetRootBone(), kShoulder );
    EXPECT_EQ( good.GetJointBone(), kElbow );
    EXPECT_EQ( good.GetEndBone(), kHand );
}

TEST( BoneControlContract, AnUnresolvableControlRefusesEveryFrameAndLeavesThePoseAlone )
{
    const Skeleton rig = MakeArmRig();

    auto broken = std::make_unique<TwoBoneIKControl>();
    broken->SetEndBone( "NoSuchBone" );

    const LocalPose plain  = PoseWith( rig, nullptr );
    const LocalPose intact = PoseWith( rig, std::move( broken ) );

    for ( uint32_t bone = 0; bone < plain.Size(); ++bone )
    {
        EXPECT_TRUE( SameBytes( plain[bone], intact[bone] ) )
             << "a control that cannot resolve its chain changed bone " << bone;
    }
}

TEST( BoneControlContract, AGoalOnTheChainsRootRefusesRatherThanRoundTrippingThePose )
{
    const Skeleton rig = MakeArmRig();

    Animator probe( rig );
    probe.Update( Common::Timestep( 1.0F / 60.0F ) );
    const glm::vec3 shoulder = glm::vec3( probe.GetBoneModelMatrix( kShoulder )[3] );

    const LocalPose plain      = PoseWith( rig, nullptr );
    const LocalPose degenerate = PoseWith( rig, MakeArmIK( rig, shoulder, 1.0F ) );

    // The solver would hand the chain straight back; applying that answer would still push three transforms
    // through decompose(inverse(parent) * compose(...)) and land a few ULPs away. Refusing keeps the pose
    // BIT-exact, which is what makes the degenerate frame diffable against the no-control frame.
    for ( uint32_t bone = 0; bone < plain.Size(); ++bone )
    {
        EXPECT_TRUE( SameBytes( plain[bone], degenerate[bone] ) )
             << "bone " << bone << " drifted on a refused solve";
    }
}

// ---------------------------------------------------------------- the pipeline around it

TEST( BoneControlContract, TheControlStageJoinsAndLeavesWithTheListAndAlwaysRunsLast )
{
    const Skeleton rig = MakeArmRig();
    Animator       animator( rig );

    ASSERT_EQ( animator.GetStages().size(), 1U );
    EXPECT_EQ( animator.GetStages()[0], PoseStage::Source );

    animator.AddControl( MakeArmIK( rig, glm::vec3( 30.0F, 70.0F, 20.0F ), 1.0F ) );
    ASSERT_EQ( animator.GetStages().size(), 2U );
    EXPECT_EQ( animator.GetStages()[1], PoseStage::Controls );

    // ORDER IS A PROPERTY OF THE PIPELINE, NOT OF THE INSERTION. A layer added after a control must still
    // run BEFORE it: a control corrects the pose the animation produced, so a layer that ran afterwards
    // would overwrite exactly the bones the control just solved.
    Desert::Animation::AnimationClip clip;
    clip.AnimationName  = "layer";
    clip.Duration       = 1.0F;
    clip.TicksPerSecond = 1.0F;
    animator.AddLayer( clip, 1.0F );

    ASSERT_EQ( animator.GetStages().size(), 3U );
    EXPECT_EQ( animator.GetStages()[0], PoseStage::Source );
    EXPECT_EQ( animator.GetStages()[1], PoseStage::Layers );
    EXPECT_EQ( animator.GetStages()[2], PoseStage::Controls );

    animator.ClearControls();
    ASSERT_EQ( animator.GetStages().size(), 2U );
    EXPECT_EQ( animator.GetStages()[1], PoseStage::Layers );
}

TEST( BoneControlContract, AControlDrivesTheRigWithNoClipPlayingAtAll )
{
    const Skeleton rig = MakeArmRig();

    // "No clip" used to be the same question as "nothing to do", and for a control it is not: the goal is
    // the input, not a track. Without this the entire feature would be unreachable for a rig standing still.
    Animator animator( rig );
    EXPECT_FALSE( animator.IsPlaying() );

    animator.AddControl( MakeArmIK( rig, glm::vec3( 30.0F, 70.0F, 20.0F ), 1.0F ) );
    animator.Update( Common::Timestep( 1.0F / 60.0F ) );

    const glm::vec3 hand = glm::vec3( animator.GetBoneModelMatrix( kHand )[3] );
    EXPECT_NEAR( glm::length( hand - glm::vec3( 30.0F, 70.0F, 20.0F ) ), 0.0F, 0.05F );
}

TEST( BoneControlContract, TheSkinningMatricesTheGPUSEESCarryTheSolve )
{
    // The pose is the currency; the skinning array is the OUTPUT, and a stage that moved the pose without
    // reaching the output would be the middle-link defect this project has found seven times in a day.
    const Skeleton rig = MakeArmRig();

    Animator plain( rig );
    plain.Update( Common::Timestep( 1.0F / 60.0F ) );
    const std::vector<glm::mat4> before = plain.GetPose().Matrices;

    Animator solved( rig );
    solved.AddControl( MakeArmIK( rig, glm::vec3( 30.0F, 70.0F, 20.0F ), 1.0F ) );
    solved.Update( Common::Timestep( 1.0F / 60.0F ) );
    const std::vector<glm::mat4>& after = solved.GetPose().Matrices;

    ASSERT_EQ( before.size(), 8U );
    ASSERT_EQ( after.size(), 8U );
    for ( uint32_t bone = 0; bone < before.size(); ++bone )
    {
        const bool shouldMove = bone == kShoulder || bone == kElbow || bone == kHand || bone == 4;
        const bool moved      = before[bone] != after[bone];
        EXPECT_EQ( moved, shouldMove ) << "skinning matrix " << bone << " ('" << rig.GetBones()[bone].Name << "') "
                                       << ( moved ? "moved" : "did not move" );
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
