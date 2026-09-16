// T5.4 — THE RIG AS A STAGE, AND THE THREE THINGS THAT HAVE TO BE TRUE FOR THAT SENTENCE TO MEAN ANYTHING.
//
//   1. A POSE THAT CAME OUT OF THE ANIMATION SOURCE COMES OUT OF THE RIG CHANGED, and the change is stated
//      as ARITHMETIC — the driven bone's component transform equals `ParentSpace * Offset * Pose`, the
//      composition T5.1 wrote — rather than as "it moved". "It moved" is also what a defect says.
//   2. WITH NO RIG, THE PIPELINE IS BIT-FOR-BIT WHAT IT WAS. This is the positive control, and it is the
//      reason `Animator::AttachRig` refuses a rig that drives no bones: without that refusal a rig which
//      silently does nothing passes test 1's "the pipeline still works" half AND test 2, and the suite
//      would be green over a stage that never runs. Tested here by DETACHING a rig that had been attached
//      and demanding the same bytes as an Animator that never had one.
//   3. THE ORDER IS NAMED. `GetStages()` is asserted as a sequence, with all four stages populated, because
//      report 05 §658's answer is specifically about ORDER: the rig runs after the AnimBP — which is our
//      Source + Layers + Controls — and an animator's authored control therefore beats what the layer put
//      on a bone they share. A pipeline where that is true by push_back accident is one merge away from
//      being false.
//
// The rig is eight bones for А2's reason: a one- or two-bone rig cannot tell "wrote the driven bone" from
// "wrote the whole pose", because the two produce the same bytes.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TwoBoneIKControl.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using Desert::Animation::AnimationClip;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTrack;
using Desert::Animation::BoneTransform;
using Desert::Animation::ControlBoneDrive;
using Desert::Animation::ControlElement;
using Desert::Animation::ControlHierarchy;
using Desert::Animation::ControlRigStage;
using Desert::Animation::ControlSpace;
using Desert::Animation::ControlSpaceKind;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::PoseStage;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::Skeleton;
using Desert::Animation::TwoBoneIKControl;

namespace
{
    // 1 world unit = 1 cm.
    //
    //   0 Spine (root) -> 1 Shoulder -> 2 Elbow -> 3 Hand -> 4 Finger
    //                  -> 5 Tail -> 6 TailTip
    //   7 Prop (a second root)
    //
    // Every bind carries a rotation as well as a translation: a rig whose bind is pure translation cannot
    // tell a bone's own space from skinning space, so it cannot catch a stage that composes them wrongly.
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

    // A skeleton the arm rig's controls do NOT fit: two bones, so a control parented to bone 7 is a rig
    // built against something else.
    Skeleton MakeStubRig()
    {
        std::vector<BoneInfo> bones( 2 );
        bones[0].Name               = "Spine";
        bones[0].LocalBindTransform = glm::mat4( 1.0F );
        bones[1].Name               = "Shoulder";
        bones[1].ParentBoneID       = 0U;
        bones[1].LocalBindTransform = glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 10.0F, 0.0F ) );

        Skeleton rig( std::move( bones ) );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    constexpr uint32_t kSpine    = 0;
    constexpr uint32_t kShoulder = 1;
    constexpr uint32_t kHand     = 3;
    constexpr uint32_t kTail     = 5;

    // A clip that moves the shoulder and the hand, so "what the animation source produced" is a real pose
    // and not the bind. Two ticks apart so the suite can scrub between two different skeletons.
    AnimationClip ArmClip()
    {
        AnimationClip clip;
        clip.AnimationName = "wave";
        clip.DurationTicks = FrameNumber{ PROJECT_TICK_RATE.Numerator };

        BoneTrack shoulder;
        shoulder.BoneName = "Shoulder";
        shoulder.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 10.0F, 20.0F, 0.0F ) } );
        shoulder.PositionKeys.push_back(
             { FrameNumber{ PROJECT_TICK_RATE.Numerator }, glm::vec3( 40.0F, 55.0F, -12.0F ) } );
        shoulder.RotationKeys.push_back( { FrameNumber{ 0 }, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) } );
        shoulder.ScaleKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 1.0F ) } );
        clip.Tracks.push_back( shoulder );

        BoneTrack hand;
        hand.BoneName = "Hand";
        hand.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 0.0F, -30.0F, 0.0F ) } );
        hand.PositionKeys.push_back(
             { FrameNumber{ PROJECT_TICK_RATE.Numerator }, glm::vec3( 7.0F, -22.0F, 4.0F ) } );
        hand.RotationKeys.push_back( { FrameNumber{ 0 }, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) } );
        hand.ScaleKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 1.0F ) } );
        clip.Tracks.push_back( hand );

        return clip;
    }

    // A clip that puts the hand somewhere the rig will then overwrite. Used as a LAYER, so the suite can
    // ask which of the two the bone ended up at.
    AnimationClip HandLayerClip()
    {
        AnimationClip clip;
        clip.AnimationName = "layer";
        clip.DurationTicks = FrameNumber{ PROJECT_TICK_RATE.Numerator };

        BoneTrack hand;
        hand.BoneName = "Hand";
        hand.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( -60.0F, 15.0F, 33.0F ) } );
        hand.RotationKeys.push_back( { FrameNumber{ 0 }, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) } );
        hand.ScaleKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 1.0F ) } );
        clip.Tracks.push_back( hand );
        return clip;
    }

    BoneTransform Placed( const glm::vec3& translation, float degrees, const glm::vec3& axis )
    {
        BoneTransform out;
        out.Translation = translation;
        out.Rotation    = glm::angleAxis( glm::radians( degrees ), glm::normalize( axis ) );
        return out;
    }

    ControlElement MakeControl( std::string name, ControlSpace parent, BoneTransform offset, BoneTransform pose )
    {
        ControlElement element;
        element.Name   = std::move( name );
        element.Offset = offset;
        element.Pose   = pose;
        element.Parents.push_back( parent );
        return element;
    }

    uint32_t MustAdd( ControlHierarchy& rig, ControlElement element )
    {
        auto added = rig.Add( std::move( element ) );
        EXPECT_TRUE( added.IsSuccess() ) << added.GetError();
        return added.IsSuccess() ? added.GetValue() : ControlHierarchy::INVALID;
    }

    ::testing::AssertionResult MatNear( const glm::mat4& a, const glm::mat4& b, float eps = 1e-3F )
    {
        for ( int c = 0; c < 4; ++c )
        {
            for ( int r = 0; r < 4; ++r )
            {
                if ( std::abs( a[c][r] - b[c][r] ) > eps )
                {
                    return ::testing::AssertionFailure()
                           << "mismatch at [" << c << "][" << r << "]: " << a[c][r] << " vs " << b[c][r];
                }
            }
        }
        return ::testing::AssertionSuccess();
    }

    bool SameBytes( const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b )
    {
        if ( a.size() != b.size() )
        {
            return false;
        }
        for ( size_t i = 0; i < a.size(); ++i )
        {
            for ( int c = 0; c < 4; ++c )
            {
                for ( int r = 0; r < 4; ++r )
                {
                    if ( a[i][c][r] != b[i][c][r] )
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    glm::vec3 Origin( const glm::mat4& m )
    {
        return glm::vec3( m[3] );
    }

    // The rig the arithmetic tests use: one control in COMPONENT space (so its parent space is exactly the
    // identity and the expected global is `Offset * Pose` with nothing else in it), driving the hand.
    std::unique_ptr<ControlRigStage> HandRig( const Skeleton& skeleton, const BoneTransform& offset,
                                              const BoneTransform& pose, uint32_t* outControl )
    {
        auto stage = std::make_unique<ControlRigStage>();
        const uint32_t control =
             MustAdd( stage->GetHierarchy(),
                      MakeControl( "Hand_CTRL", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F }, offset,
                                   pose ) );
        if ( outControl != nullptr )
        {
            *outControl = control;
        }
        const auto drives = stage->SetDrives( skeleton, { ControlBoneDrive{ control, kHand } } );
        EXPECT_TRUE( drives.IsSuccess() ) << drives.GetError();
        return stage;
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 1. The pose goes through the rig and comes out changed, and the change is arithmetic.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigStageTest, ThePoseFromTheAnimationSourceComesOutOfTheRigAtTheControlsGlobalTransform )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();

    Animator animator( skeleton );
    animator.Play( clip, false );
    animator.SetTick( FrameTime{ FrameNumber{ 0 } } );

    const glm::mat4 handWithoutRig = animator.GetBoneModelMatrix( kHand );

    const BoneTransform offset = Placed( { 5.0F, 0.0F, -3.0F }, 12.0F, { 0.0F, 1.0F, 0.0F } );
    // FAR FROM WHERE THE CLIP PUTS THE HAND, on purpose. The arithmetic assertion below is the exact one;
    // this distance exists so that "changed" is a number with room above the float noise (~1e-4 cm on a
    // 100-cm rig) rather than a difference an unrelated tweak to the clip could close.
    const BoneTransform pose   = Placed( { 150.0F, -120.0F, 80.0F }, -35.0F, { 1.0F, 0.0F, 1.0F } );

    uint32_t   control = ControlHierarchy::INVALID;
    const auto attached = animator.AttachRig( HandRig( skeleton, offset, pose, &control ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();

    animator.SetTick( FrameTime{ FrameNumber{ 0 } } );
    ASSERT_TRUE( animator.GetRig()->GetLastError().empty() ) << animator.GetRig()->GetLastError();

    // T5.1's one line, spelled out: a Component-parented control's parent space is the identity, so the
    // global the rig hands the bone is exactly `Offset * Pose`.
    const glm::mat4 expected = offset.ToMatrix() * pose.ToMatrix();
    EXPECT_TRUE( MatNear( animator.GetBoneModelMatrix( kHand ), expected ) );
    EXPECT_TRUE( MatNear( animator.GetRig()->GetHierarchy().GetGlobalTransform( control ), expected ) );

    // AND IT IS A DIFFERENT POSE, BY A STATED DISTANCE. Without this the test above would also pass on a
    // rig that happened to reproduce the animation, which is the "silently does nothing" shape.
    const float moved = glm::length( Origin( animator.GetBoneModelMatrix( kHand ) ) - Origin( handWithoutRig ) );
    EXPECT_GT( moved, 100.0F ) << "the hand moved " << moved << " cm, which is not a rig doing anything";
}

TEST( ControlRigStageTest, TheRigWritesTheBoneItDrivesAndNoOtherBone )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();

    Animator plain( skeleton );
    plain.Play( clip, false );
    plain.SetTick( FrameTime{ FrameNumber{ 0 } } );

    Animator rigged( skeleton );
    rigged.Play( clip, false );
    const auto attached = rigged.AttachRig( HandRig( skeleton, Placed( { 1.0F, 2.0F, 3.0F }, 5.0F, { 0, 1, 0 } ),
                                                     Placed( { 42.0F, 77.0F, -18.0F }, -35.0F, { 1, 0, 1 } ),
                                                     nullptr ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();
    rigged.SetTick( FrameTime{ FrameNumber{ 0 } } );

    // SPARSENESS IS THE CONTRACT THE OUTPUT HOP INHERITS from `ApplyBoneOverrides`. Every bone that is not
    // the driven one keeps the LOCAL transform the animation gave it — including the Finger, whose
    // component transform legitimately moves because its parent did.
    for ( uint32_t bone = 0; bone < skeleton.GetBones().size(); ++bone )
    {
        const bool driven = ( bone == kHand );
        const auto& a     = plain.GetLocalPose()[bone];
        const auto& b     = rigged.GetLocalPose()[bone];
        const bool same   = a.Translation == b.Translation && a.Rotation == b.Rotation && a.Scale == b.Scale;
        EXPECT_EQ( same, !driven ) << "bone " << bone << " ('" << skeleton.GetBones()[bone].Name << "')";
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 2. The positive control: with no rig, nothing moved.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigStageTest, WithNoRigThePipelineIsBitForBitWhatItWasBeforeTheStageExisted )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();

    Animator untouched( skeleton );
    untouched.Play( clip, false );
    untouched.SetTick( FrameTime{ FrameNumber{ 7 } } );
    const std::vector<glm::mat4> reference = untouched.GetPose().Matrices;
    ASSERT_EQ( untouched.GetStages(), std::vector<PoseStage>( { PoseStage::Source } ) );

    Animator cycled( skeleton );
    cycled.Play( clip, false );
    const auto attached = cycled.AttachRig( HandRig( skeleton, Placed( { 5.0F, 0.0F, -3.0F }, 12.0F, { 0, 1, 0 } ),
                                                     Placed( { 42.0F, 77.0F, -18.0F }, -35.0F, { 1, 0, 1 } ),
                                                     nullptr ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();
    cycled.SetTick( FrameTime{ FrameNumber{ 7 } } );
    ASSERT_FALSE( SameBytes( cycled.GetPose().Matrices, reference ) ) << "the rig did nothing, so this "
                                                                         "suite's positive control proves "
                                                                         "nothing";

    cycled.DetachRig();
    cycled.SetTick( FrameTime{ FrameNumber{ 7 } } );

    EXPECT_EQ( cycled.GetStages(), std::vector<PoseStage>( { PoseStage::Source } ) );
    EXPECT_TRUE( SameBytes( cycled.GetPose().Matrices, reference ) );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 3. The order is named, not implied.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigStageTest, TheRigIsTheLastStageAndTheWholeOrderIsAsserted )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();
    const AnimationClip layer    = HandLayerClip();

    Animator animator( skeleton );
    animator.Play( clip, false );
    EXPECT_EQ( animator.GetStages(), std::vector<PoseStage>( { PoseStage::Source } ) );

    // ADDED IN THE WRONG ORDER ON PURPOSE: rig, then control, then layer. `SyncStages` rebuilds the list in
    // the pipeline's canonical order, so the sequence below must not depend on the order of these calls.
    const auto attached = animator.AttachRig( HandRig( skeleton, BoneTransform{},
                                                       Placed( { 42.0F, 77.0F, -18.0F }, -35.0F, { 1, 0, 1 } ),
                                                       nullptr ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();

    auto ik = std::make_unique<TwoBoneIKControl>();
    ik->SetEndBone( "Hand" );
    ASSERT_GE( animator.AddControl( std::move( ik ) ), 0 );

    ASSERT_GE( animator.AddLayer( layer, 1.0F, false, false ), 0 );

    EXPECT_EQ( animator.GetStages(), std::vector<PoseStage>( { PoseStage::Source, PoseStage::Layers,
                                                               PoseStage::Controls, PoseStage::Rig } ) );
}

TEST( ControlRigStageTest, TheRigRunsAfterTheLayerAndWinsOnABoneTheyShare )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();
    const AnimationClip layer    = HandLayerClip();

    const BoneTransform pose = Placed( { 42.0F, 77.0F, -18.0F }, -35.0F, { 1.0F, 0.0F, 1.0F } );

    // First: the layer alone really does move the hand, so "the rig won" is a statement about two claimants
    // and not about a layer that was never applied.
    Animator layerOnly( skeleton );
    layerOnly.Play( clip, false );
    ASSERT_GE( layerOnly.AddLayer( layer, 1.0F, false, false ), 0 );
    layerOnly.SetTick( FrameTime{ FrameNumber{ 0 } } );
    const glm::mat4 handFromLayer = layerOnly.GetBoneModelMatrix( kHand );

    Animator both( skeleton );
    both.Play( clip, false );
    ASSERT_GE( both.AddLayer( layer, 1.0F, false, false ), 0 );
    const auto attached = both.AttachRig( HandRig( skeleton, BoneTransform{}, pose, nullptr ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();
    both.SetTick( FrameTime{ FrameNumber{ 0 } } );

    EXPECT_TRUE( MatNear( both.GetBoneModelMatrix( kHand ), pose.ToMatrix() ) );
    const float apart = glm::length( Origin( both.GetBoneModelMatrix( kHand ) ) - Origin( handFromLayer ) );
    EXPECT_GT( apart, 100.0F ) << "the layer and the rig disagree by only " << apart
                               << " cm, which is not enough to tell which one ran last";
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// The input hop: the rig reads THIS frame's bones.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigStageTest, AControlParentedToABoneFollowsThisFramesPoseAndNotThePreviousOnes )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();

    // The control sits exactly ON the shoulder (identity offset and pose) and drives the TAIL, which the
    // clip does not touch and which is not in the shoulder's chain. So "the tail is where the shoulder is"
    // is a statement about the input hop, and the clip moving the shoulder makes it a different statement
    // on every tick.
    auto           stage   = std::make_unique<ControlRigStage>();
    const uint32_t control = MustAdd( stage->GetHierarchy(),
                                      MakeControl( "Shoulder_CTRL",
                                                   ControlSpace{ ControlSpaceKind::Bone, kShoulder, 1.0F },
                                                   BoneTransform{}, BoneTransform{} ) );
    const auto drives = stage->SetDrives( skeleton, { ControlBoneDrive{ control, kTail } } );
    ASSERT_TRUE( drives.IsSuccess() ) << drives.GetError();

    Animator animator( skeleton );
    animator.Play( clip, false );
    const auto attached = animator.AttachRig( std::move( stage ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();

    animator.SetTick( FrameTime{ FrameNumber{ 0 } } );
    const glm::mat4 tailAtZero     = animator.GetBoneModelMatrix( kTail );
    const glm::mat4 shoulderAtZero = animator.GetBoneModelMatrix( kShoulder );
    EXPECT_TRUE( MatNear( tailAtZero, shoulderAtZero ) );

    animator.SetTick( FrameTime{ FrameNumber{ PROJECT_TICK_RATE.Numerator } } );
    const glm::mat4 tailAtEnd     = animator.GetBoneModelMatrix( kTail );
    const glm::mat4 shoulderAtEnd = animator.GetBoneModelMatrix( kShoulder );
    EXPECT_TRUE( MatNear( tailAtEnd, shoulderAtEnd ) );

    const float travelled = glm::length( Origin( tailAtEnd ) - Origin( tailAtZero ) );
    EXPECT_GT( travelled, 10.0F ) << "the driven bone travelled " << travelled
                                  << " cm between the two ticks — a rig reading a stale pose would sit still";
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// The output hop's ordering, and the refusals.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigStageTest, DrivesAreSortedParentsFirstSoTheOutputHopIsNeverHandedAnUnorderedList )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();

    const BoneTransform shoulderPose = Placed( { -30.0F, 60.0F, 11.0F }, 20.0F, { 0.0F, 0.0F, 1.0F } );
    const BoneTransform handPose     = Placed( { 42.0F, 77.0F, -18.0F }, -35.0F, { 1.0F, 0.0F, 1.0F } );

    auto           stage = std::make_unique<ControlRigStage>();
    const uint32_t hand  = MustAdd( stage->GetHierarchy(),
                                    MakeControl( "Hand_CTRL", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F },
                                                 BoneTransform{}, handPose ) );
    const uint32_t shoulder =
         MustAdd( stage->GetHierarchy(),
                  MakeControl( "Shoulder_CTRL", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F },
                               BoneTransform{}, shoulderPose ) );

    // CHILD FIRST. `ApplyBoneOverrides` refuses this order, naming both resolve ranks; the rig must sort it
    // rather than pass the author's order through and fail for a reason the author cannot see.
    const auto drives =
         stage->SetDrives( skeleton, { ControlBoneDrive{ hand, kHand }, ControlBoneDrive{ shoulder, kShoulder } } );
    ASSERT_TRUE( drives.IsSuccess() ) << drives.GetError();
    ASSERT_EQ( stage->GetDrives().size(), 2U );
    EXPECT_EQ( stage->GetDrives()[0].Bone, kShoulder );
    EXPECT_EQ( stage->GetDrives()[1].Bone, kHand );

    Animator animator( skeleton );
    animator.Play( clip, false );
    const auto attached = animator.AttachRig( std::move( stage ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();
    animator.SetTick( FrameTime{ FrameNumber{ 0 } } );

    ASSERT_TRUE( animator.GetRig()->GetLastError().empty() ) << animator.GetRig()->GetLastError();
    // BOTH land on their controls, and the hand's is the interesting one: it is a DESCENDANT of a bone the
    // same pass moved, so it only lands there if it was converted against the shoulder's solved transform.
    EXPECT_TRUE( MatNear( animator.GetBoneModelMatrix( kShoulder ), shoulderPose.ToMatrix() ) );
    EXPECT_TRUE( MatNear( animator.GetBoneModelMatrix( kHand ), handPose.ToMatrix() ) );
}

TEST( ControlRigStageTest, TwoControlsDrivingOneBoneAreRefusedRatherThanSettledBySortOrder )
{
    const Skeleton skeleton = MakeArmRig();

    ControlRigStage stage;
    const uint32_t  first =
         MustAdd( stage.GetHierarchy(), MakeControl( "A", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F },
                                                     BoneTransform{}, BoneTransform{} ) );
    const uint32_t second =
         MustAdd( stage.GetHierarchy(), MakeControl( "B", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F },
                                                     BoneTransform{}, BoneTransform{} ) );

    const auto refused =
         stage.SetDrives( skeleton, { ControlBoneDrive{ first, kHand }, ControlBoneDrive{ second, kHand } } );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Hand" ), std::string::npos ) << refused.GetError();
    EXPECT_TRUE( stage.GetDrives().empty() );
}

TEST( ControlRigStageTest, ADriveNamingABoneThisSkeletonHasNotIsRefusedAtAuthoringTime )
{
    const Skeleton skeleton = MakeStubRig(); // two bones

    ControlRigStage stage;
    const uint32_t  control =
         MustAdd( stage.GetHierarchy(), MakeControl( "A", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F },
                                                     BoneTransform{}, BoneTransform{} ) );

    const auto refused = stage.SetDrives( skeleton, { ControlBoneDrive{ control, kTail } } );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "different skeleton" ), std::string::npos ) << refused.GetError();
}

TEST( ControlRigStageTest, ARigThatDrivesNothingIsRefusedRatherThanAttachedAsAnIdentityStage )
{
    const Skeleton skeleton = MakeArmRig();

    // A rig with controls but no drives: exactly the thing that would join `GetStages()` and pass every
    // assertion in this file except the ones that measure a change.
    auto stage = std::make_unique<ControlRigStage>();
    MustAdd( stage->GetHierarchy(), MakeControl( "A", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F },
                                                 BoneTransform{}, BoneTransform{} ) );

    Animator   animator( skeleton );
    const auto refused = animator.AttachRig( std::move( stage ) );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_EQ( animator.GetRig(), nullptr );
    EXPECT_EQ( animator.GetStages(), std::vector<PoseStage>( { PoseStage::Source } ) );

    const auto nullRefused = animator.AttachRig( nullptr );
    EXPECT_FALSE( nullRefused.IsSuccess() );

    ControlRigStage bare;
    const auto      emptyRefused = bare.SetDrives( skeleton, {} );
    EXPECT_FALSE( emptyRefused.IsSuccess() );
}

TEST( ControlRigStageTest, ARigOverTheWrongSkeletonRefusesAndLeavesThePoseAsTheStageBeforeItProducedIt )
{
    const Skeleton      big      = MakeArmRig();
    const Skeleton      small    = MakeStubRig();
    const AnimationClip clip     = ArmClip();

    // Built and validated against the eight-bone rig: a control parented to bone 7, driving bone 0.
    auto           stage   = std::make_unique<ControlRigStage>();
    const uint32_t control = MustAdd( stage->GetHierarchy(),
                                      MakeControl( "Prop_CTRL", ControlSpace{ ControlSpaceKind::Bone, 7, 1.0F },
                                                   BoneTransform{},
                                                   Placed( { 500.0F, 0.0F, 0.0F }, 0.0F, { 0, 1, 0 } ) ) );
    const auto drives = stage->SetDrives( big, { ControlBoneDrive{ control, kSpine } } );
    ASSERT_TRUE( drives.IsSuccess() ) << drives.GetError();

    Animator reference( small );
    reference.Play( clip, false );
    reference.SetTick( FrameTime{ FrameNumber{ 0 } } );
    const std::vector<glm::mat4> before = reference.GetPose().Matrices;

    Animator animator( small );
    animator.Play( clip, false );
    const auto attached = animator.AttachRig( std::move( stage ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();
    animator.SetTick( FrameTime{ FrameNumber{ 0 } } );

    EXPECT_FALSE( animator.GetRig()->GetLastError().empty() );
    EXPECT_NE( animator.GetRig()->GetLastError().find( "different skeleton" ), std::string::npos )
         << animator.GetRig()->GetLastError();
    // A REFUSAL IS NOT A HALF-APPLIED POSE. The stage ran and contributed nothing, which is the only
    // honest answer a rig that cannot read the skeleton has.
    EXPECT_TRUE( SameBytes( animator.GetPose().Matrices, before ) );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
