// KEYING A CONTROL, AND THE TWO BEHAVIOURS THAT ARE THE TASK.
//
// T5.3. Writing a control's pose into a clip is the easy part and the suite spends four assertions on it;
// the rest is about the two rules, and each of them is asserted WITH ITS POSITIVE CONTROL, because both
// have the same failure mode — a keyer that never writes anything passes "only one key appeared" and
// "playback wrote nothing" perfectly.
//
//   * report 05 §971, deferral to interaction end: an interaction spanning N ticks leaves ONE key, and
//     the same N writes with no interaction open leave N. Without the second half the first is vacuous.
//   * report 01 §823, the "never key" flag: `ApplyClipToControls` moves every control and adds no key,
//     and the same poses written as `Authored` at the same tick do add one.
//
// The placement claim is ARITHMETIC and not a picture: the key holds the control's LOCAL pose, so
// `ParentSpace * Offset * keyed` has to be the global the hierarchy reports. That is checkable; "the
// control looked like it was in the right place" is not, and there is no viewport in this suite anyway.

#include <Engine/Animation/Rig/ControlKeyer.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <utility>

using Desert::Animation::AnimationClip;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTrack;
using Desert::Animation::BoneTransform;
using Desert::Animation::ComponentPose;
using Desert::Animation::ControlElement;
using Desert::Animation::ControlHierarchy;
using Desert::Animation::ControlKeyer;
using Desert::Animation::ControlKeyTarget;
using Desert::Animation::ControlSpace;
using Desert::Animation::ControlSpaceKind;
using Desert::Animation::ControlWriteSource;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::KeyInterp;
using Desert::Animation::LocalPose;
using Desert::Animation::Skeleton;
using Desert::Animation::TangentMode;

namespace
{
    /// Two bones so that a control can hang off one that is NOT at the origin — a parent space of identity
    /// would let a wrong composition order pass every placement assertion in this file.
    Skeleton MakeSkeleton()
    {
        std::vector<BoneInfo> bones;
        BoneInfo              root;
        root.Name = "root";
        bones.push_back( root );

        BoneInfo chest;
        chest.Name         = "chest";
        chest.ParentBoneID = 0;
        bones.push_back( chest );

        return Skeleton( std::move( bones ) );
    }

    LocalPose ChestAt( const glm::vec3& where )
    {
        LocalPose pose;
        pose.Resize( 2 );
        pose[1].Translation = where;
        return pose;
    }

    ControlElement MakeControl( const char* name, std::vector<ControlSpace> parents )
    {
        ControlElement element;
        element.Name    = name;
        element.Parents = std::move( parents );
        return element;
    }

    BoneTransform TransformOf( const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale )
    {
        BoneTransform out;
        out.Translation = translation;
        out.Rotation    = rotation;
        out.Scale       = scale;
        return out;
    }

    AnimationClip MakeClip( int durationTicks )
    {
        AnimationClip clip;
        clip.AnimationName = "take";
        clip.DurationTicks = FrameNumber{ durationTicks };
        return clip;
    }

    const BoneTrack* FindTrack( const AnimationClip& clip, const char* name )
    {
        for ( const BoneTrack& track : clip.Tracks )
        {
            if ( track.BoneName == name )
            {
                return &track;
            }
        }
        return nullptr;
    }

    /// The whole rig this suite keys against, kept in one object so a test reads as the scenario it is.
    struct Fixture
    {
        Skeleton         Bones = MakeSkeleton();
        LocalPose        Local = ChestAt( glm::vec3( 0.0F, 100.0F, 0.0F ) );
        ComponentPose    Pose{ Bones, Local };
        ControlHierarchy Rig;
        AnimationClip    Clip = MakeClip( 240 );
        ControlKeyer     Keyer;
        uint32_t         Hand = ControlHierarchy::INVALID;

        Fixture()
        {
            ControlElement hand = MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } } );
            // AN OFFSET THAT IS NOT IDENTITY, on purpose: the key records the pose and NOT the offset, so
            // an implementation that keyed `Offset * Pose` would pass with a zero offset and be wrong for
            // every rig a person would actually author.
            hand.Offset.Translation = glm::vec3( 10.0F, 0.0F, 0.0F );
            const auto added        = Rig.Add( hand );
            Hand                    = added.IsSuccess() ? added.GetValue() : ControlHierarchy::INVALID;
            Rig.Evaluate( Bones, Pose );
        }

        /// The buffer the gizmo poses and a BONE key is read from. Separate from `Local` on purpose:
        /// `Local` is what the rig was evaluated against, and a keyer that read the evaluated pose would
        /// pass every assertion here while keying the wrong buffer in the editor.
        LocalPose Authoring = ChestAt( glm::vec3( 0.0F, 100.0F, 0.0F ) );

        [[nodiscard]] ControlKeyTarget At( int tick )
        {
            ControlKeyTarget target;
            target.Hierarchy    = &Rig;
            target.Skeleton     = &Bones;
            target.Clip         = &Clip;
            target.Tick         = FrameNumber{ tick };
            target.AuthoredPose = &Authoring;
            return target;
        }
    };
} // namespace

// ── WHERE THE KEY LANDS, AND ON WHICH TICK ───────────────────────────────────────────────────────────

TEST( ControlKeying, KeyHoldsTheControlsLocalPoseOnTheRequestedTick )
{
    Fixture fix;

    const BoneTransform pose =
         TransformOf( glm::vec3( 3.0F, -4.0F, 5.0F ), glm::angleAxis( 0.7F, glm::vec3( 0.0F, 1.0F, 0.0F ) ),
                      glm::vec3( 2.0F, 0.5F, 1.25F ) );

    const auto written = fix.Keyer.Write( fix.At( 96 ), fix.Hand, pose, ControlWriteSource::Authored );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    EXPECT_EQ( written.GetValue(), 1U );

    const BoneTrack* track = FindTrack( fix.Clip, "hand_ctrl" );
    ASSERT_NE( track, nullptr ) << "a control's keys live in a track named after the control";
    ASSERT_EQ( track->PositionKeys.size(), 1U );
    ASSERT_EQ( track->RotationKeys.size(), 1U );
    ASSERT_EQ( track->ScaleKeys.size(), 1U );

    EXPECT_EQ( track->PositionKeys[0].Tick.Value, 96 );
    EXPECT_EQ( track->RotationKeys[0].Tick.Value, 96 );
    EXPECT_EQ( track->ScaleKeys[0].Tick.Value, 96 );

    // BIT-EXACT, not near: nothing between the control and the key is allowed to be arithmetic.
    EXPECT_EQ( track->PositionKeys[0].Position, pose.Translation );
    EXPECT_EQ( track->ScaleKeys[0].Scale, pose.Scale );
    EXPECT_EQ( track->RotationKeys[0].Rotation, pose.Rotation );
}

TEST( ControlKeying, TheKeyIsWhereTheControlIs )
{
    Fixture fix;

    const BoneTransform pose =
         TransformOf( glm::vec3( 3.0F, -4.0F, 5.0F ), glm::angleAxis( 0.7F, glm::vec3( 0.0F, 1.0F, 0.0F ) ),
                      glm::vec3( 1.0F ) );
    ASSERT_TRUE( fix.Keyer.Write( fix.At( 96 ), fix.Hand, pose, ControlWriteSource::Authored ).IsSuccess() );

    const glm::mat4 global = fix.Rig.GetGlobalTransform( fix.Hand );

    // THE ARITHMETIC. The parent space is the chest's global (the control declares one bone slot at weight
    // 1), the offset is the rigger's, and the keyed value is the third factor. Recomposing them from the
    // KEY has to give the transform the hierarchy reports — which is the claim "the key is where the
    // control is", written down as multiplication.
    const BoneTrack* track = FindTrack( fix.Clip, "hand_ctrl" );
    ASSERT_NE( track, nullptr );
    const BoneTransform sampled = track->Sample( FrameTime{ FrameNumber{ 96 }, 0.0F }, fix.Clip.TickRate );

    const glm::mat4 chest      = fix.Pose.Get( 1 );
    const glm::mat4 recomposed = chest * fix.Rig.Get( fix.Hand ).Offset.ToMatrix() * sampled.ToMatrix();

    for ( int column = 0; column < 4; ++column )
    {
        for ( int row = 0; row < 4; ++row )
        {
            EXPECT_NEAR( recomposed[column][row], global[column][row], 1e-4F )
                 << "column " << column << " row " << row;
        }
    }

    // And the number a person would read off the screen: the chest is 100 up, the offset is 10 along x,
    // and the pose is (3, -4, 5) — so the control, and therefore the key, is at (13, 96, 5).
    EXPECT_NEAR( global[3][0], 13.0F, 1e-4F );
    EXPECT_NEAR( global[3][1], 96.0F, 1e-4F );
    EXPECT_NEAR( global[3][2], 5.0F, 1e-4F );
}

// ── REPORT 05 §971: ONE KEY PER INTERACTION, AND THE POSITIVE CONTROL ────────────────────────────────

namespace
{
    /// The eight frames of a drag: the pointer moves and the playhead moves with it, which is what
    /// happens when a control is dragged while the sequence plays. `interaction` is the only difference
    /// between the two halves of the claim below.
    constexpr int kDragFrames = 8;

    void DriveDrag( Fixture& fix, bool interaction )
    {
        if ( interaction )
        {
            ASSERT_TRUE( fix.Keyer.BeginInteraction().IsSuccess() );
        }
        for ( int frame = 0; frame < kDragFrames; ++frame )
        {
            const BoneTransform pose = TransformOf( glm::vec3( static_cast<float>( frame ), 0.0F, 0.0F ),
                                                    glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );
            const auto written = fix.Keyer.Write( fix.At( frame ), fix.Hand, pose, ControlWriteSource::Authored );
            ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
            EXPECT_EQ( written.GetValue(), interaction ? 0U : 1U );
        }
        if ( interaction )
        {
            const auto ended = fix.Keyer.EndInteraction( fix.At( kDragFrames - 1 ) );
            ASSERT_TRUE( ended.IsSuccess() ) << ended.GetError();
            EXPECT_EQ( ended.GetValue(), 1U ) << "one key per control, whatever the drag did";
        }
    }
} // namespace

TEST( ControlKeying, AnInteractionSpanningEightTicksLeavesOneKey )
{
    Fixture fix;
    DriveDrag( fix, true );

    const BoneTrack* track = FindTrack( fix.Clip, "hand_ctrl" );
    ASSERT_NE( track, nullptr );
    EXPECT_EQ( track->PositionKeys.size(), 1U );
    EXPECT_EQ( track->RotationKeys.size(), 1U );
    EXPECT_EQ( track->ScaleKeys.size(), 1U );

    // AT THE TICK THE INTERACTION ENDED, holding the value it ended AT — not the first intermediate the
    // pointer passed through, which is what a keyer remembering the value at `Write` would have stored.
    EXPECT_EQ( track->PositionKeys[0].Tick.Value, kDragFrames - 1 );
    EXPECT_FLOAT_EQ( track->PositionKeys[0].Position.x, static_cast<float>( kDragFrames - 1 ) );
}

TEST( ControlKeying, WithoutTheDeferralTheSameDragLeavesEightKeys )
{
    // THE POSITIVE CONTROL for the test above. Without it, a keyer that wrote nothing at all would pass
    // "exactly one key appeared" — and so would one whose `Write` silently refused every call.
    Fixture fix;
    DriveDrag( fix, false );

    const BoneTrack* track = FindTrack( fix.Clip, "hand_ctrl" );
    ASSERT_NE( track, nullptr );
    EXPECT_EQ( track->PositionKeys.size(), static_cast<size_t>( kDragFrames ) );
    EXPECT_EQ( track->RotationKeys.size(), static_cast<size_t>( kDragFrames ) );
    EXPECT_EQ( track->ScaleKeys.size(), static_cast<size_t>( kDragFrames ) );
    for ( int frame = 0; frame < kDragFrames; ++frame )
    {
        EXPECT_EQ( track->PositionKeys[static_cast<size_t>( frame )].Tick.Value, frame );
    }
}

TEST( ControlKeying, TheClipIsUntouchedUntilTheInteractionEnds )
{
    Fixture fix;
    ASSERT_TRUE( fix.Keyer.BeginInteraction().IsSuccess() );
    ASSERT_TRUE( fix.Keyer
                      .Write( fix.At( 4 ), fix.Hand,
                              TransformOf( glm::vec3( 1.0F, 2.0F, 3.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                                           glm::vec3( 1.0F ) ),
                              ControlWriteSource::Authored )
                      .IsSuccess() );

    EXPECT_EQ( fix.Keyer.Pending(), 1U );
    EXPECT_TRUE( fix.Clip.Tracks.empty() ) << "a deferred key must not even create the track";

    // The POSE, however, is written immediately — the control is on screen under the animator's pointer,
    // and a manipulator whose control only moved on mouse-up would be unusable.
    EXPECT_EQ( fix.Rig.Get( fix.Hand ).Pose.Translation, glm::vec3( 1.0F, 2.0F, 3.0F ) );
}

TEST( ControlKeying, ACancelledInteractionKeysNothing )
{
    Fixture fix;
    ASSERT_TRUE( fix.Keyer.BeginInteraction().IsSuccess() );
    ASSERT_TRUE(
         fix.Keyer
              .Write( fix.At( 4 ), fix.Hand,
                      TransformOf( glm::vec3( 1.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ),
                      ControlWriteSource::Authored )
              .IsSuccess() );
    fix.Keyer.CancelInteraction();

    EXPECT_FALSE( fix.Keyer.Interacting() );
    EXPECT_EQ( fix.Keyer.Pending(), 0U );
    EXPECT_TRUE( fix.Clip.Tracks.empty() );
    EXPECT_FALSE( fix.Keyer.EndInteraction( fix.At( 4 ) ).IsSuccess() )
         << "a cancelled interaction is closed, not waiting";
}

TEST( ControlKeying, RepeatedInteractionsOnOneTickUpsertRatherThanAccumulate )
{
    // The other half of §971's shape: with the playhead STILL, keying twice is one key holding the second
    // value. A key count is not the observable here — a value is.
    Fixture fix;
    for ( int pass = 1; pass <= 3; ++pass )
    {
        ASSERT_TRUE( fix.Keyer.BeginInteraction().IsSuccess() );
        ASSERT_TRUE( fix.Keyer
                          .Write( fix.At( 12 ), fix.Hand,
                                  TransformOf( glm::vec3( static_cast<float>( pass ), 0.0F, 0.0F ),
                                               glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ),
                                  ControlWriteSource::Authored )
                          .IsSuccess() );
        ASSERT_TRUE( fix.Keyer.EndInteraction( fix.At( 12 ) ).IsSuccess() );
    }

    const BoneTrack* track = FindTrack( fix.Clip, "hand_ctrl" );
    ASSERT_NE( track, nullptr );
    ASSERT_EQ( track->PositionKeys.size(), 1U );
    EXPECT_FLOAT_EQ( track->PositionKeys[0].Position.x, 3.0F );
}

// ── REPORT 01 §823: PLAYBACK DOES NOT FEED THE KEYER, AND THE POSITIVE CONTROL ──────────────────────

namespace
{
    /// Every key in the clip, flattened, so "nothing was written" can be asserted as an equality rather
    /// than as a count that a re-key of the same value would satisfy.
    std::vector<std::pair<int, glm::vec3>> PositionKeysOf( const AnimationClip& clip )
    {
        std::vector<std::pair<int, glm::vec3>> out;
        for ( const BoneTrack& track : clip.Tracks )
        {
            for ( const auto& key : track.PositionKeys )
            {
                out.emplace_back( key.Tick.Value, key.Position );
            }
        }
        return out;
    }
} // namespace

TEST( ControlKeying, PlaybackMovesEveryControlAndWritesNoKey )
{
    Fixture fix;

    // TWO authored keys, and playback scrubs BETWEEN them. The first version of this test authored one
    // key and scrubbed onto it, and a mutation that deleted the `Playback` branch entirely PASSED it: the
    // re-key wrote the same value onto the same tick, so nothing observable moved. A keyer feeding itself
    // is only visible where the sampled value is one the clip does not already hold a key for — which is
    // every frame of an actual playback, and was the one frame that test did not use.
    for ( const int tick : { 0, 60 } )
    {
        ASSERT_TRUE( fix.Keyer
                          .Write( fix.At( tick ), fix.Hand,
                                  TransformOf( glm::vec3( static_cast<float>( tick ), 0.0F, 0.0F ),
                                               glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ),
                                  ControlWriteSource::Authored )
                          .IsSuccess() );
    }
    const auto before = PositionKeysOf( fix.Clip );
    ASSERT_EQ( before.size(), 2U );

    // Move the control away, then scrub to a tick BETWEEN the keys: this is the loop §823 closes.
    ASSERT_TRUE( fix.Rig
                      .SetPose( fix.Hand, TransformOf( glm::vec3( -50.0F, 0.0F, 0.0F ),
                                                       glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ) )
                      .IsSuccess() );
    const auto applied = Desert::Animation::ApplyClipToControls( fix.At( 30 ), fix.Keyer );
    ASSERT_TRUE( applied.IsSuccess() ) << applied.GetError();
    EXPECT_EQ( applied.GetValue(), 1U ) << "playback has to actually move the control";

    // IT MOVED, and to a value no key holds — otherwise "no key was written" is the answer a no-op gives.
    const float sampled = fix.Rig.Get( fix.Hand ).Pose.Translation.x;
    EXPECT_GT( sampled, 0.0F );
    EXPECT_LT( sampled, 60.0F );

    // AND NOTHING WAS WRITTEN. Not "the same number of keys" — the same keys.
    const auto after = PositionKeysOf( fix.Clip );
    ASSERT_EQ( after.size(), before.size() );
    for ( size_t i = 0; i < before.size(); ++i )
    {
        EXPECT_EQ( after[i].first, before[i].first );
        EXPECT_EQ( after[i].second, before[i].second );
    }
}

TEST( ControlKeying, TheSamePoseWrittenAsAuthoredDoesKey )
{
    // THE POSITIVE CONTROL for the test above, on the same tick and with the same value, so the only
    // difference between keying and not keying is the flag.
    Fixture             fix;
    const BoneTransform pose =
         TransformOf( glm::vec3( 6.0F, 7.0F, 8.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );

    ASSERT_TRUE( fix.Keyer.Write( fix.At( 30 ), fix.Hand, pose, ControlWriteSource::Playback ).IsSuccess() );
    EXPECT_TRUE( fix.Clip.Tracks.empty() ) << "the flag is what stops this";

    const auto written = fix.Keyer.Write( fix.At( 30 ), fix.Hand, pose, ControlWriteSource::Authored );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    EXPECT_EQ( written.GetValue(), 1U );
    ASSERT_NE( FindTrack( fix.Clip, "hand_ctrl" ), nullptr );
    EXPECT_EQ( FindTrack( fix.Clip, "hand_ctrl" )->PositionKeys.size(), 1U );
}

TEST( ControlKeying, PlaybackDuringAnInteractionDoesNotBecomePending )
{
    // The case where both rules are true at once: the animator is holding a control while the clip plays.
    // A `Playback` write that merely DEFERRED would be keyed by the end of the drag, which is the loop
    // arriving one frame later rather than being closed.
    Fixture fix;
    ASSERT_TRUE( fix.Keyer.BeginInteraction().IsSuccess() );
    ASSERT_TRUE(
         fix.Keyer
              .Write( fix.At( 10 ), fix.Hand,
                      TransformOf( glm::vec3( 1.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ),
                      ControlWriteSource::Playback )
              .IsSuccess() );
    EXPECT_EQ( fix.Keyer.Pending(), 0U );

    const auto ended = fix.Keyer.EndInteraction( fix.At( 10 ) );
    ASSERT_TRUE( ended.IsSuccess() ) << ended.GetError();
    EXPECT_EQ( ended.GetValue(), 0U );
    EXPECT_TRUE( fix.Clip.Tracks.empty() );
}

TEST( ControlKeying, PlaybackLeavesAnUnkeyedControlWhereItIs )
{
    Fixture              fix;
    const ControlElement extra =
         MakeControl( "elbow_ctrl", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    const auto added = fix.Rig.Add( extra );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    const uint32_t elbow = added.GetValue();
    fix.Rig.Evaluate( fix.Bones, fix.Pose );

    ASSERT_TRUE( fix.Rig
                      .SetPose( elbow, TransformOf( glm::vec3( 42.0F, 0.0F, 0.0F ),
                                                    glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ) )
                      .IsSuccess() );
    ASSERT_TRUE(
         fix.Keyer
              .Write( fix.At( 5 ), fix.Hand,
                      TransformOf( glm::vec3( 1.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ),
                      ControlWriteSource::Authored )
              .IsSuccess() );

    const auto applied = Desert::Animation::ApplyClipToControls( fix.At( 5 ), fix.Keyer );
    ASSERT_TRUE( applied.IsSuccess() ) << applied.GetError();
    EXPECT_EQ( applied.GetValue(), 1U ) << "only the control with a track was moved";
    EXPECT_EQ( fix.Rig.Get( elbow ).Pose.Translation, glm::vec3( 42.0F, 0.0F, 0.0F ) )
         << "an unkeyed control has no animation, and no animation is not 'at the origin'";
}

// ── THE SCALE DECISION A9 ASKED FOR ─────────────────────────────────────────────────────────────────

TEST( ControlKeying, ANonUniformScaleKeysIntoTheScaleChannelAndReadsBack )
{
    Fixture         fix;
    const glm::vec3 scale( 2.0F, 0.5F, 3.0F );
    ASSERT_TRUE( fix.Keyer
                      .Write( fix.At( 60 ), fix.Hand,
                              TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), scale ),
                              ControlWriteSource::Authored )
                      .IsSuccess() );

    const BoneTrack* track = FindTrack( fix.Clip, "hand_ctrl" );
    ASSERT_NE( track, nullptr );
    ASSERT_EQ( track->ScaleKeys.size(), 1U );
    // THREE FLOATS, not one. A uniform-only scale control would make two thirds of this channel
    // unauthorable while the clip was perfectly able to hold it.
    EXPECT_EQ( track->ScaleKeys[0].Scale, scale );
    EXPECT_EQ( track->ScaleKeys[0].Interp, KeyInterp::Cubic );
    EXPECT_EQ( track->ScaleKeys[0].Mode, TangentMode::Auto );

    const BoneTransform sampled = track->Sample( FrameTime{ FrameNumber{ 60 }, 0.0F }, fix.Clip.TickRate );
    EXPECT_NEAR( sampled.Scale.x, 2.0F, 1e-5F );
    EXPECT_NEAR( sampled.Scale.y, 0.5F, 1e-5F );
    EXPECT_NEAR( sampled.Scale.z, 3.0F, 1e-5F );
}

TEST( ControlKeying, AScaledControlScalesItsChildrensSpaces )
{
    // NOT A DECISION T5.3 MADE. T5.1 composes `ParentSpace * Offset * Pose` as matrices, so a child whose
    // parent is a control inherits that control's scale by construction. A9's second open question is
    // answered by this arithmetic rather than by a paragraph, and if the composition ever changes this is
    // the test that says so.
    Fixture        fix;
    ControlElement child   = MakeControl( "finger_ctrl", { ControlSpace{ ControlSpaceKind::Control, 0, 1.0F } } );
    child.Pose.Translation = glm::vec3( 4.0F, 0.0F, 0.0F );
    const auto added       = fix.Rig.Add( child );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    const uint32_t finger = added.GetValue();
    fix.Rig.Evaluate( fix.Bones, fix.Pose );

    ASSERT_TRUE( fix.Rig
                      .SetPose( fix.Hand, TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                                                       glm::vec3( 3.0F, 1.0F, 1.0F ) ) )
                      .IsSuccess() );

    const glm::mat4 parent = fix.Rig.GetGlobalTransform( fix.Hand );
    const glm::mat4 global = fix.Rig.GetGlobalTransform( finger );

    // The chest is 100 up, the hand's offset is 10 along x, the hand scales x by three, and the finger
    // sits 4 along x inside it: 10 + 3 * 4 = 22.
    EXPECT_NEAR( global[3][0], 22.0F, 1e-4F );
    EXPECT_NEAR( global[3][1], 100.0F, 1e-4F );

    const glm::mat4 recomposed =
         parent * fix.Rig.Get( finger ).Offset.ToMatrix() * fix.Rig.Get( finger ).Pose.ToMatrix();
    for ( int column = 0; column < 4; ++column )
    {
        for ( int row = 0; row < 4; ++row )
        {
            EXPECT_NEAR( recomposed[column][row], global[column][row], 1e-4F );
        }
    }
}

// ── AN UPSERT DOES NOT DISCARD THE ANIMATOR'S CURVE ─────────────────────────────────────────────────

TEST( ControlKeying, RekeyingAnExistingKeyKeepsItsAuthoredShape )
{
    Fixture fix;
    ASSERT_TRUE( fix.Keyer
                      .Write( fix.At( 20 ), fix.Hand,
                              TransformOf( glm::vec3( 1.0F, 0.0F, 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                                           glm::vec3( 1.0F ) ),
                              ControlWriteSource::Authored )
                      .IsSuccess() );

    // The animator breaks the tangent and holds the pose — work the next nudge must not throw away, for
    // the same reason `AutoSetTangents` leaves a `User` key alone.
    BoneTrack* track = nullptr;
    for ( BoneTrack& candidate : fix.Clip.Tracks )
    {
        if ( candidate.BoneName == "hand_ctrl" )
        {
            track = &candidate;
        }
    }
    ASSERT_NE( track, nullptr );
    track->PositionKeys[0].Interp        = KeyInterp::Constant;
    track->PositionKeys[0].Mode          = TangentMode::User;
    track->PositionKeys[0].ArriveTangent = glm::vec3( 9.0F, 0.0F, 0.0F );
    track->PositionKeys[0].LeaveTangent  = glm::vec3( -9.0F, 0.0F, 0.0F );

    ASSERT_TRUE( fix.Keyer
                      .Write( fix.At( 20 ), fix.Hand,
                              TransformOf( glm::vec3( 5.0F, 0.0F, 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                                           glm::vec3( 1.0F ) ),
                              ControlWriteSource::Authored )
                      .IsSuccess() );

    ASSERT_EQ( track->PositionKeys.size(), 1U );
    EXPECT_FLOAT_EQ( track->PositionKeys[0].Position.x, 5.0F ) << "the value is what a re-key changes";
    EXPECT_EQ( track->PositionKeys[0].Interp, KeyInterp::Constant );
    EXPECT_EQ( track->PositionKeys[0].Mode, TangentMode::User );
    EXPECT_FLOAT_EQ( track->PositionKeys[0].ArriveTangent.x, 9.0F );
    EXPECT_FLOAT_EQ( track->PositionKeys[0].LeaveTangent.x, -9.0F );
}

TEST( ControlKeying, AKeyInsertedBeforeExistingOnesStaysSorted )
{
    // `lower_bound` in the sampler decides which two keys bracket the playhead, so an unsorted channel is
    // not a cosmetic problem: it is a sampler reading the wrong pair.
    Fixture fix;
    for ( const int tick : { 100, 40, 70, 10 } )
    {
        ASSERT_TRUE( fix.Keyer
                          .Write( fix.At( tick ), fix.Hand,
                                  TransformOf( glm::vec3( static_cast<float>( tick ), 0.0F, 0.0F ),
                                               glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ),
                                  ControlWriteSource::Authored )
                          .IsSuccess() );
    }

    const BoneTrack* track = FindTrack( fix.Clip, "hand_ctrl" );
    ASSERT_NE( track, nullptr );
    ASSERT_EQ( track->PositionKeys.size(), 4U );
    EXPECT_EQ( track->PositionKeys[0].Tick.Value, 10 );
    EXPECT_EQ( track->PositionKeys[1].Tick.Value, 40 );
    EXPECT_EQ( track->PositionKeys[2].Tick.Value, 70 );
    EXPECT_EQ( track->PositionKeys[3].Tick.Value, 100 );
    EXPECT_FLOAT_EQ( track->PositionKeys[2].Position.x, 70.0F ) << "sorted by tick, values travelling with";
}

// ── REFUSALS ────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlKeying, AControlNamedLikeABoneIsRefused )
{
    const Skeleton  bones = MakeSkeleton();
    const LocalPose local = ChestAt( glm::vec3( 0.0F, 100.0F, 0.0F ) );
    ComponentPose   pose{ bones, local };

    ControlHierarchy rig;
    const auto added = rig.Add( MakeControl( "chest", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } ) );
    ASSERT_TRUE( added.IsSuccess() );
    rig.Evaluate( bones, pose );

    AnimationClip clip = MakeClip( 240 );
    ControlKeyer  keyer;

    ControlKeyTarget target;
    target.Hierarchy = &rig;
    target.Skeleton  = &bones;
    target.Clip      = &clip;
    target.Tick      = FrameNumber{ 0 };

    const auto written =
         keyer.Write( target, added.GetValue(),
                      TransformOf( glm::vec3( 1.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ),
                      ControlWriteSource::Authored );
    EXPECT_FALSE( written.IsSuccess() ) << "a track name is the only binding key: these keys would drive the bone";
    EXPECT_TRUE( clip.Tracks.empty() );
    // AND THE POSE IS NOT STORED EITHER: animation under a control that can never be keyed is work the
    // animator loses without being told.
    EXPECT_EQ( rig.Get( added.GetValue() ).Pose.Translation, glm::vec3( 0.0F ) );
}

TEST( ControlKeying, RefusalsThatWouldOtherwiseBeSilent )
{
    Fixture             fix;
    const BoneTransform pose =
         TransformOf( glm::vec3( 1.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );

    EXPECT_FALSE( fix.Keyer.Write( fix.At( 241 ), fix.Hand, pose, ControlWriteSource::Authored ).IsSuccess() )
         << "a key past the clip's end is a key nothing samples";
    EXPECT_FALSE( fix.Keyer.Write( fix.At( -1 ), fix.Hand, pose, ControlWriteSource::Authored ).IsSuccess() );
    EXPECT_TRUE( fix.Keyer.Write( fix.At( 240 ), fix.Hand, pose, ControlWriteSource::Authored ).IsSuccess() )
         << "the last tick of the clip is inside it";

    EXPECT_FALSE( fix.Keyer.Write( fix.At( 0 ), 99U, pose, ControlWriteSource::Authored ).IsSuccess() );

    ControlKeyTarget broken = fix.At( 0 );
    broken.Clip             = nullptr;
    EXPECT_FALSE( fix.Keyer.Write( broken, fix.Hand, pose, ControlWriteSource::Authored ).IsSuccess() );

    EXPECT_FALSE( fix.Keyer.EndInteraction( fix.At( 0 ) ).IsSuccess() )
         << "an end with no beginning would key at whatever tick the playhead is on";
    ASSERT_TRUE( fix.Keyer.BeginInteraction().IsSuccess() );
    EXPECT_FALSE( fix.Keyer.BeginInteraction().IsSuccess() ) << "interactions do not nest";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ── THE SAME TWO RULES FOR A BONE (A28) ──────────────────────────────────────────────────────────────
//
// THE NUMBER THIS FILE EXISTS TO STATE. The Sequencer's Record mode keys bones, not controls, and it
// had no interaction: it called a keying routine once per mouse-move frame in which the posed bone
// differed. Two facts decide what that cost, and they pull in opposite directions:
//
//   * the write is an UPSERT ON A TICK, so N writes at ONE tick leave ONE key per channel — the
//     per-mouse-move loop was NOT producing a key per mouse move, and saying it was would be wrong;
//   * an interaction that CROSSES ticks leaves one key per tick crossed, and nothing in the keyer
//     stops that. `kRecordFrames` frames across `kRecordFrames` ticks is the measurement below.
//
// Both halves are asserted, because either alone is misleading: the first without the second says the
// defect never existed, the second without the first inflates it.

namespace
{
    /// One second of dragging at 60 Hz. The playhead advances with it, which is what "scrub or play
    /// while holding the gizmo" does — the case the deferral makes unreachable by construction.
    constexpr int kRecordFrames = 60;

    /// Move the posed bone as a drag would, and offer it to the keyer once per frame.
    /// @param interaction whether the drag is bracketed by Begin/EndInteraction.
    /// @param movePlayhead whether the playhead advances during the drag.
    /// @return how many keys the keyer reported writing across the whole drag.
    uint32_t DriveBoneDrag( Fixture& fix, bool interaction, bool movePlayhead )
    {
        if ( interaction )
        {
            EXPECT_TRUE( fix.Keyer.BeginInteraction().IsSuccess() );
        }
        uint32_t keyed = 0;
        for ( int frame = 0; frame < kRecordFrames; ++frame )
        {
            fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, static_cast<float>( frame ) );
            const int  tick    = movePlayhead ? frame : 0;
            const auto written = fix.Keyer.WriteBone( fix.At( tick ), 1 );
            EXPECT_TRUE( written.IsSuccess() ) << written.GetError();
            keyed += written.IsSuccess() ? written.GetValue() : 0;
        }
        if ( interaction )
        {
            const auto ended = fix.Keyer.EndInteraction( fix.At( movePlayhead ? kRecordFrames - 1 : 0 ) );
            EXPECT_TRUE( ended.IsSuccess() ) << ended.GetError();
            keyed += ended.IsSuccess() ? ended.GetValue() : 0;
        }
        return keyed;
    }

    size_t PositionKeyCount( const AnimationClip& clip, const char* track )
    {
        const BoneTrack* found = FindTrack( clip, track );
        return found == nullptr ? 0U : found->PositionKeys.size();
    }
} // namespace

TEST( ControlKeying, AFrozenPlayheadAlreadyCollapsedSixtyWritesOntoOneKey )
{
    // THE "BEFORE" NUMBER, AND IT IS NOT THE ONE THE GAP ANALYSIS PREDICTED. With the playhead where the
    // Sequencer holds it during a drag, the undeferred loop wrote sixty times and left one key: the tick
    // upsert had been doing the deduplication all along. A claim of "a key per mouse move" is false here,
    // and it matters that it is said in a test rather than in a report nobody can re-run.
    Fixture        fix;
    const uint32_t keyed = DriveBoneDrag( fix, /*interaction=*/false, /*movePlayhead=*/false );

    EXPECT_EQ( keyed, static_cast<uint32_t>( kRecordFrames ) ) << "sixty keying calls did happen";
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 1U ) << "and left one key, because they shared a tick";
}

TEST( ControlKeying, AnUndeferredDragThatCrossesTicksLeavesOneKeyPerTick )
{
    // THE POSITIVE CONTROL FOR THE TEST BELOW, and the defect's real size. The moment the playhead moves
    // under the drag, the upsert stops collapsing anything and the clip records the path of the pointer.
    Fixture        fix;
    const uint32_t keyed = DriveBoneDrag( fix, /*interaction=*/false, /*movePlayhead=*/true );

    EXPECT_EQ( keyed, static_cast<uint32_t>( kRecordFrames ) );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), static_cast<size_t>( kRecordFrames ) )
         << "sixty ticks, sixty keys — this is what report 05 §971 calls a correctness defect";
}

TEST( ControlKeying, TheSameDragInsideAnInteractionLeavesOneKey )
{
    Fixture        fix;
    const uint32_t keyed = DriveBoneDrag( fix, /*interaction=*/true, /*movePlayhead=*/true );

    EXPECT_EQ( keyed, 1U ) << "one key per subject, whatever the drag and the playhead did";

    const BoneTrack* track = FindTrack( fix.Clip, "chest" );
    ASSERT_NE( track, nullptr ) << "a bone's keys live in a track named after the bone";
    EXPECT_EQ( track->PositionKeys.size(), 1U );
    EXPECT_EQ( track->RotationKeys.size(), 1U );
    EXPECT_EQ( track->ScaleKeys.size(), 1U );
    EXPECT_EQ( track->PositionKeys[0].Tick.Value, kRecordFrames - 1 ) << "at the tick the drag ended on";

    // RESOLVED AT THE COMMIT, NOT REMEMBERED AT THE WRITE: the value is the one the drag ENDED at, and a
    // keyer that stored the pose handed to the first `WriteBone` would put frame 0 here.
    EXPECT_EQ( track->PositionKeys[0].Position.z, static_cast<float>( kRecordFrames - 1 ) );
}

TEST( ControlKeying, ABoneAndAControlShareOneInteraction )
{
    // The pending list is keyed on {kind, index}, so bone 0 and control 0 are two subjects. THE INDICES
    // ARE DELIBERATELY EQUAL: with a bone 1 and a control 0 this test passed even when `KeySubject`'s
    // equality ignored `Kind` entirely — a degenerate scenario, and it was caught by the mutation rather
    // than by reading it (§8.4). With both at 0, an implementation that stored bare indices folds the two
    // subjects into one and keys only the first.
    Fixture fix;
    ASSERT_EQ( fix.Hand, 0U ) << "this test's whole point is that the two indices collide";
    ASSERT_TRUE( fix.Keyer.BeginInteraction().IsSuccess() );

    fix.Authoring[0].Translation = glm::vec3( 1.0F, 2.0F, 3.0F );
    ASSERT_TRUE( fix.Keyer.WriteBone( fix.At( 20 ), 0 ).IsSuccess() );
    ASSERT_TRUE( fix.Keyer
                      .Write( fix.At( 20 ), fix.Hand,
                              TransformOf( glm::vec3( 9.0F, 0.0F, 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                                           glm::vec3( 1.0F ) ),
                              ControlWriteSource::Authored )
                      .IsSuccess() );
    EXPECT_EQ( fix.Keyer.Pending(), 2U ) << "a bone and a control are two subjects, not one index";

    const auto ended = fix.Keyer.EndInteraction( fix.At( 20 ) );
    ASSERT_TRUE( ended.IsSuccess() ) << ended.GetError();
    EXPECT_EQ( ended.GetValue(), 2U );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "root" ), 1U );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "hand_ctrl" ), 1U );
}

TEST( ControlKeying, ABoneKeyWithoutAnAuthoringPoseIsRefusedRatherThanKeyedFromTheBindPose )
{
    Fixture          fix;
    ControlKeyTarget target = fix.At( 10 );
    target.AuthoredPose     = nullptr;

    const auto written = fix.Keyer.WriteBone( target, 1 );
    EXPECT_FALSE( written.IsSuccess() ) << "the bind pose is not the animator's work — see §936";
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 0U );

    EXPECT_FALSE( fix.Keyer.WriteBone( fix.At( 10 ), 99 ).IsSuccess() ) << "no bone 99 in a skeleton of 2";
}

TEST( ControlKeying, AControlNamedAfterABoneIsStillRefusedButTheBoneItselfIsNot )
{
    // The collision refusal is about a CONTROL borrowing a bone's name. Generalising `Check` over subjects
    // is exactly where that rule could have been turned on the bone it is meant to protect, which would
    // make every bone in the tree unkeyable — and every count-based test above would still pass.
    Fixture fix;
    ASSERT_TRUE( fix.Keyer.WriteBone( fix.At( 10 ), 1 ).IsSuccess() );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 1U );

    ControlElement clash = MakeControl( "chest", { ControlSpace{ ControlSpaceKind::Bone, 0, 1.0F } } );
    const auto     added = fix.Rig.Add( clash );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();

    const auto written =
         fix.Keyer.Write( fix.At( 10 ), added.GetValue(),
                          TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ),
                          ControlWriteSource::Authored );
    EXPECT_FALSE( written.IsSuccess() ) << "a control named after a bone would drive that bone";
}

// ── THE KEYING VOCABULARY (report 05 §939), AND THE DEFAULT THAT IS A FACT ABOUT UE ──────────────────
//
// Two enums, not three. `EAllowEditsMode` is refused in `ControlKeyer.hpp` with the count that refuses
// it; there is nothing here to assert about an enum that does not exist, which is the point.
//
// Every mode below is asserted WITH ITS POSITIVE CONTROL, for the reason the top of this file gives: a
// keyer that wrote nothing at all would pass "auto-key off writes nothing" perfectly.

namespace
{
    using Desert::Animation::AutoChangeMode;
    using Desert::Animation::KeyGroupMode;
    using Desert::Animation::KeyingModes;
    using Desert::Animation::KeySubject;
    using Desert::Animation::KeySubjectKind;

    constexpr KeySubject kChest{ KeySubjectKind::Bone, 1 };

    /// A held drag over `kRecordFrames` frames driven entirely through `Observe`, released at the end.
    /// Returns the keys the keyer reported writing across the whole gesture.
    uint32_t ObserveDrag( Fixture& fix, bool movePlayhead = true )
    {
        uint32_t keyed = 0;
        for ( int frame = 0; frame < kRecordFrames; ++frame )
        {
            fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, static_cast<float>( frame ) );
            const auto written =
                 fix.Keyer.Observe( fix.At( movePlayhead ? frame : 0 ), kChest, /*pointerHeld=*/true,
                                    /*subjectMoved=*/true );
            EXPECT_TRUE( written.IsSuccess() ) << written.GetError();
            keyed += written.IsSuccess() ? written.GetValue() : 0;
        }
        const auto released = fix.Keyer.Observe( fix.At( movePlayhead ? kRecordFrames - 1 : 0 ), kChest,
                                                 /*pointerHeld=*/false, /*subjectMoved=*/false );
        EXPECT_TRUE( released.IsSuccess() ) << released.GetError();
        return keyed + ( released.IsSuccess() ? released.GetValue() : 0 );
    }

    KeyingModes With( AutoChangeMode change, KeyGroupMode group = KeyGroupMode::Subject )
    {
        KeyingModes modes;
        modes.AutoChange = change;
        modes.KeyGroup   = group;
        return modes;
    }
} // namespace

TEST( ControlKeying, AutoKeyShipsOffAndADragUnderItChangesNothingOnDisk )
{
    Fixture fix;
    EXPECT_EQ( fix.Keyer.Modes().AutoChange, AutoChangeMode::None )
         << "UE ships auto-key off; a default of 'convenient' is a clip an animator did not agree to";

    EXPECT_EQ( ObserveDrag( fix ), 0U );
    EXPECT_EQ( fix.Clip.Tracks.size(), 0U ) << "not even an empty track: nothing was recording";
}

TEST( ControlKeying, TheSameDragWithAutoKeyOnLeavesExactlyOneKey )
{
    // THE POSITIVE CONTROL for the test above AND the whole of Record mode driven through one call, which
    // is the part `SequencerPanel.cpp` is no longer allowed to spell for itself.
    Fixture fix;
    fix.Keyer.SetModes( With( AutoChangeMode::All ) );

    EXPECT_EQ( ObserveDrag( fix ), 1U ) << "sixty held frames across sixty ticks, one key";

    const BoneTrack* track = FindTrack( fix.Clip, "chest" );
    ASSERT_NE( track, nullptr );
    ASSERT_EQ( track->PositionKeys.size(), 1U );
    EXPECT_EQ( track->PositionKeys[0].Tick.Value, kRecordFrames - 1 ) << "at the tick the pointer let go";
    EXPECT_EQ( track->PositionKeys[0].Position.z, static_cast<float>( kRecordFrames - 1 ) );
}

TEST( ControlKeying, SwitchingAutoKeyOffMidDragCommitsNothingWhenThePointerIsReleased )
{
    // The failure this forbids is a drag that was never being recorded committing a key on release
    // because an interaction was still open from before the mode changed.
    Fixture fix;
    fix.Keyer.SetModes( With( AutoChangeMode::All ) );

    fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, 1.0F );
    ASSERT_TRUE( fix.Keyer.Observe( fix.At( 4 ), kChest, true, true ).IsSuccess() );
    EXPECT_EQ( fix.Keyer.Pending(), 1U );

    fix.Keyer.SetModes( With( AutoChangeMode::None ) );
    const auto released = fix.Keyer.Observe( fix.At( 6 ), kChest, false, false );
    ASSERT_TRUE( released.IsSuccess() ) << released.GetError();
    EXPECT_EQ( released.GetValue(), 0U );
    EXPECT_FALSE( fix.Keyer.Interacting() ) << "the abandoned interaction is closed, not left open";
    EXPECT_EQ( fix.Clip.Tracks.size(), 0U );
}

TEST( ControlKeying, AnExplicitKeyIsNotSilencedByAutoKeyBeingOff )
{
    // `AutoChangeMode` speaks for the AUTOMATIC path only. A mode named "auto-key off" that also disabled
    // the Key button would be a button that does nothing, with no way to find out why.
    Fixture fix;
    ASSERT_EQ( fix.Keyer.Modes().AutoChange, AutoChangeMode::None );

    fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, 7.0F );
    const auto written = fix.Keyer.WriteBone( fix.At( 12 ), 1 );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    EXPECT_EQ( written.GetValue(), 1U );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 1U );
}

TEST( ControlKeying, AutoKeyLeavesASubjectWithNoTrackAloneAndKeysOneThatHasOne )
{
    Fixture fix;
    fix.Keyer.SetModes( With( AutoChangeMode::AutoKey ) );

    EXPECT_EQ( ObserveDrag( fix ), 0U ) << "'key what is already animated' does not start animating it";
    EXPECT_EQ( fix.Clip.Tracks.size(), 0U ) << "and does not leave an empty track behind either";

    // POSITIVE CONTROL: give the bone a track by hand, then drag again.
    fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, 0.0F );
    ASSERT_TRUE( fix.Keyer.WriteBone( fix.At( 0 ), 1 ).IsSuccess() );
    EXPECT_EQ( ObserveDrag( fix ), 1U );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 2U ) << "the seeded key, and the one the drag added";
}

TEST( ControlKeying, AutoTrackCreatesTheTrackAndWritesNoKey )
{
    Fixture fix;
    fix.Keyer.SetModes( With( AutoChangeMode::AutoTrack ) );

    EXPECT_EQ( ObserveDrag( fix ), 0U );
    const BoneTrack* track = FindTrack( fix.Clip, "chest" );
    ASSERT_NE( track, nullptr ) << "the track is what this mode DOES do";
    EXPECT_FALSE( track->HasKeys() ) << "and the key is what it withholds";
}

TEST( ControlKeying, KeyGroupChangedSkipsASubjectTheCurveAlreadyAgreesWith )
{
    Fixture fix;
    fix.Keyer.SetModes( With( AutoChangeMode::All, KeyGroupMode::Changed ) );

    // Two keys holding the same value, so the curve says that value everywhere between them.
    fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, 5.0F );
    ASSERT_TRUE( fix.Keyer.WriteBone( fix.At( 0 ), 1 ).IsSuccess() );
    ASSERT_TRUE( fix.Keyer.WriteBone( fix.At( 40 ), 1 ).IsSuccess() );
    ASSERT_EQ( PositionKeyCount( fix.Clip, "chest" ), 2U );

    // Held still at tick 20: the clip already says this, so nothing is pinned into the middle of it.
    const auto still = fix.Keyer.Observe( fix.At( 20 ), kChest, /*pointerHeld=*/true, /*subjectMoved=*/true );
    ASSERT_TRUE( still.IsSuccess() ) << still.GetError();
    ASSERT_TRUE( fix.Keyer.Observe( fix.At( 20 ), kChest, false, false ).IsSuccess() );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 2U ) << "an agreed-with pose is not an edit";

    // POSITIVE CONTROL: move it, and the same gesture at the same tick does key.
    fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, 9.0F );
    ASSERT_TRUE( fix.Keyer.Observe( fix.At( 20 ), kChest, true, true ).IsSuccess() );
    ASSERT_TRUE( fix.Keyer.Observe( fix.At( 20 ), kChest, false, false ).IsSuccess() );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 3U );
}

TEST( ControlKeying, KeyGroupAllKeysEveryBoneOfTheRigAndNotTheControls )
{
    // "Everything" means the KINDS the interaction touched. A drag on a bone that also keyed the rig's
    // controls would be a gesture writing tracks for a hierarchy the animator was not working in.
    Fixture fix;
    fix.Keyer.SetModes( With( AutoChangeMode::All, KeyGroupMode::All ) );

    fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, 3.0F );
    ASSERT_TRUE( fix.Keyer.Observe( fix.At( 8 ), kChest, true, true ).IsSuccess() );
    const auto released = fix.Keyer.Observe( fix.At( 8 ), kChest, false, false );
    ASSERT_TRUE( released.IsSuccess() ) << released.GetError();

    EXPECT_EQ( released.GetValue(), 2U ) << "both bones of this skeleton, from one bone being moved";
    EXPECT_EQ( PositionKeyCount( fix.Clip, "root" ), 1U );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 1U );
    EXPECT_EQ( PositionKeyCount( fix.Clip, "hand_ctrl" ), 0U ) << "no control was touched by this gesture";
}

TEST( ControlKeying, ABoneIsKeyableWithNoControlRigAtAll )
{
    // THE EDITOR'S ORDINARY CASE, and it was refused until it was asked for: a plain skinned character has
    // no `ControlHierarchy`, and a target requiring one would have made every Sequencer bone key in the
    // tree fail with a message about a rig the animator was not using.
    Fixture          fix;
    ControlKeyTarget target = fix.At( 16 );
    target.Hierarchy        = nullptr;

    fix.Authoring[1].Translation = glm::vec3( 0.0F, 100.0F, 4.0F );
    const auto written           = fix.Keyer.WriteBone( target, 1 );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    EXPECT_EQ( PositionKeyCount( fix.Clip, "chest" ), 1U );

    // …and the control half still refuses, naming the missing hierarchy rather than crashing on it.
    EXPECT_FALSE( fix.Keyer.Write( target, 0,
                                   TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                                                glm::vec3( 1.0F ) ),
                                   ControlWriteSource::Authored )
                       .IsSuccess() );
}
