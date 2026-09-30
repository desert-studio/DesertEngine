// ONE UNDO PATH FOR ANIMATION DATA: SequenceEditTransaction / SequenceEditCommand over a clip's and a UI
// animation's Timeline::Sequence (Editor/Core/Commands/SequenceEdit.hpp).

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/PoseEditTransaction.hpp>
#include <Editor/Core/Commands/SequenceEdit.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlKeyer.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Engine/ECS/Components.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace
{
    namespace Animation = Desert::Animation;
    namespace Timeline  = Desert::Animation::Timeline;
    using Desert::Editor::CommandHistory;
    using Desert::Editor::OwnerOf;
    using Desert::Editor::SameStoredValue;
    using Desert::Editor::ScopedSequenceEdit;
    using Desert::Editor::SequenceEditCommand;
    using Desert::Editor::SequenceEditTransaction;

    constexpr uint32_t kChild = 1;
    constexpr int32_t  kDisplayFrameTicks =
         Animation::PROJECT_TICK_RATE.Numerator / Animation::DEFAULT_DISPLAY_RATE.Numerator;

    Animation::Skeleton MakeChain()
    {
        std::vector<Animation::BoneInfo> bones( 2 );
        bones[0].Name               = "root";
        bones[0].ParentBoneID       = std::nullopt;
        bones[0].LocalBindTransform = glm::mat4( 1.0f );
        bones[1].Name               = "child";
        bones[1].ParentBoneID       = 0u;
        bones[1].LocalBindTransform = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        Animation::Skeleton skeleton( std::move( bones ) );
        skeleton.RecomputeOffsetMatrices();
        return skeleton;
    }

    Animation::AnimationClip MakeClip()
    {
        Animation::AnimationClip clip;
        clip.AnimationName  = "take01";
        clip.Sequence.Start = Animation::FrameNumber{ 0 };
        clip.Sequence.End   = Animation::FrameNumber{ Animation::PROJECT_TICK_RATE.Numerator };
        return clip;
    }

    Animation::BoneTransform At( float y )
    {
        Animation::BoneTransform pose;
        pose.Translation = glm::vec3( 0.0f, y, 0.0f );
        return pose;
    }

    const SequenceEditCommand* Top()
    {
        const auto& stack = CommandHistory::Get().UndoStack();
        return stack.empty() ? nullptr : dynamic_cast<const SequenceEditCommand*>( stack.back().get() );
    }

    class ClipEditUndo : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            CommandHistory::Get().Clear();
            m_Animator.ApplyLocalPose();
        }
        void TearDown() override
        {
            CommandHistory::Get().Clear();
        }

        void Pose( float y )
        {
            Animation::LocalPose pose = m_Animator.GetAuthoringPose();
            pose[kChild]              = At( y );
            ASSERT_TRUE( m_Animator.SetAuthoringPose( pose ).IsSuccess() );
        }

        /// What the Sequencer keys into this frame: the clip at the playhead, the rig when there is one.
        [[nodiscard]] Animation::ControlKeyTarget Target( Animation::ControlHierarchy* hierarchy )
        {
            Animation::ControlKeyTarget target;
            target.Hierarchy    = hierarchy;
            target.Skeleton     = &m_Skeleton;
            target.Clip         = &m_Clip;
            target.AuthoredPose = &m_Animator.GetAuthoringPose();
            target.Tick         = m_Tick;
            return target;
        }

        void AutoKey( Animation::AutoChangeMode change )
        {
            Animation::KeyingModes modes;
            modes.AutoChange = change;
            modes.KeyGroup   = Animation::KeyGroupMode::Subject;
            m_Keyer.SetModes( modes );
        }

        Animation::Skeleton      m_Skeleton = MakeChain();
        Animation::Animator      m_Animator{ m_Skeleton };
        Animation::AnimationClip m_Clip = MakeClip();
        SequenceEditTransaction  m_Transaction;
        Animation::ControlKeyer  m_Keyer;
        Animation::FrameNumber   m_Tick{ kDisplayFrameTicks * 15 };
    };
} // namespace

TEST_F( ClipEditUndo, KeyBoneIsOneEntryAndUndoRemovesTheTrackItCreatedAndRedoPutsItBackByValue )
{
    const Timeline::Sequence before = m_Clip.Sequence;
    Pose( 4.0f );
    const auto keyed =
         Desert::Editor::KeyBonePose( m_Transaction, &m_Animator, &m_Clip, kChild, Animation::FrameNumber{ 0 } );
    ASSERT_TRUE( keyed.IsSuccess() ) << keyed.GetError();
    ASSERT_EQ( keyed.GetValue(), 1U );
    ASSERT_NE( Top(), nullptr );
    EXPECT_EQ( Top()->ChangedTracks(), 1U );
    EXPECT_TRUE( Top()->CarriesHeader() ) << "the bone binding is part of the header";
    const Timeline::Sequence after = m_Clip.Sequence;

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( m_Clip.Sequence, before ) );
    EXPECT_TRUE( m_Clip.Sequence.Tracks.empty() );
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_TRUE( SameStoredValue( m_Clip.Sequence, after ) );
}

TEST_F( ClipEditUndo, FortyFramesOfEditsInOneTransactionAreOneUndoStepAndUndoRestoresPoseAndClip )
{
    ASSERT_TRUE(
         Animation::SetBoneKey( m_Clip.Sequence, "child", Animation::FrameNumber{ 0 }, At( 1.0f ) ).IsSuccess() );
    const Timeline::Sequence   before     = m_Clip.Sequence;
    const Animation::LocalPose poseBefore = m_Animator.GetAuthoringPose();

    ASSERT_TRUE( m_Transaction.Begin( OwnerOf( &m_Clip ), &m_Animator ).IsSuccess() );
    for ( int frame = 1; frame <= 40; ++frame )
    {
        Pose( static_cast<float>( frame ) );
        ASSERT_TRUE( Animation::SetBoneKey( m_Clip.Sequence, "child", Animation::FrameNumber{ 0 },
                                            At( static_cast<float>( frame ) ) )
                          .IsSuccess() );
    }
    const auto ended = m_Transaction.End();
    ASSERT_TRUE( ended.IsSuccess() ) << ended.GetError();
    EXPECT_EQ( ended.GetValue(), 1U );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1U );
    ASSERT_NE( Top(), nullptr );
    EXPECT_EQ( Top()->ChangedBones(), 1U );
    EXPECT_TRUE( Top()->IsVolatile() );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( m_Clip.Sequence, before ) );
    EXPECT_TRUE( SameStoredValue( m_Animator.GetAuthoringPose(), poseBefore ) );
}

TEST_F( ClipEditUndo, ASectionEditThroughTheTrackFunctionsIsUndoneByValueAndBumpsTheRevision )
{
    ASSERT_TRUE(
         Animation::SetBoneKey( m_Clip.Sequence, "child", Animation::FrameNumber{ 0 }, At( 1.0f ) ).IsSuccess() );
    const Timeline::Sequence before = m_Clip.Sequence;
    {
        ScopedSequenceEdit edit( m_Transaction, OwnerOf( &m_Clip ) );
        Timeline::Track&   track = m_Clip.Sequence.Tracks.front();
        ASSERT_TRUE( Timeline::SetSectionRow( track, 0, 3 ).IsSuccess() );
    }
    ASSERT_NE( Top(), nullptr );
    EXPECT_EQ( Top()->ChangedTracks(), 1U );
    EXPECT_FALSE( Top()->CarriesHeader() );
    const uint32_t revision = m_Clip.Sequence.Revision;
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( m_Clip.Sequence, before ) );
    EXPECT_GT( m_Clip.Sequence.Revision, revision );
}

TEST_F( ClipEditUndo, AnInteractionThatChangedNothingIsNotAStepAndTransactionsDoNotNest )
{
    ASSERT_TRUE( m_Transaction.Begin( OwnerOf( &m_Clip ), &m_Animator ).IsSuccess() );
    EXPECT_FALSE( m_Transaction.Begin( OwnerOf( &m_Clip ) ).IsSuccess() );
    EXPECT_EQ( m_Transaction.Subject(), &m_Clip );
    const auto ended = m_Transaction.End();
    ASSERT_TRUE( ended.IsSuccess() );
    EXPECT_EQ( ended.GetValue(), 0U );
    EXPECT_TRUE( CommandHistory::Get().UndoStack().empty() );
    EXPECT_FALSE(
         m_Transaction.Begin( OwnerOf( static_cast<Animation::AnimationClip*>( nullptr ) ) ).IsSuccess() );
}

TEST_F( ClipEditUndo, AnEdgeDrivenDragTakesLastFramesPoseAsItsBefore )
{
    Pose( 2.0f );
    const auto owner = OwnerOf( &m_Clip );
    ASSERT_TRUE( m_Transaction.Observe( owner, &m_Animator, false ).IsSuccess() );
    Pose( 5.0f ); // the manipulator writes the pose in the same frame the bit rises
    ASSERT_TRUE( m_Transaction.Observe( owner, &m_Animator, true ).IsSuccess() );
    EXPECT_TRUE( m_Transaction.Open() );
    EXPECT_FALSE( m_Transaction.OpenExplicitly() );
    Pose( 9.0f );
    const auto released = m_Transaction.Observe( owner, &m_Animator, false );
    ASSERT_TRUE( released.IsSuccess() );
    EXPECT_EQ( released.GetValue(), 1U );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( m_Animator.GetAuthoringPose()[kChild], At( 2.0f ) ) );
}

TEST_F( ClipEditUndo, DroppingAPreviewAnimatorsRecordsKeepsTheSequenceOnlyRecords )
{
    {
        ScopedSequenceEdit edit( m_Transaction, OwnerOf( &m_Clip ) );
        ASSERT_TRUE( Animation::SetBoneKey( m_Clip.Sequence, "child", Animation::FrameNumber{ 0 }, At( 1.0f ) )
                          .IsSuccess() );
    }
    Pose( 3.0f );
    const auto keyed =
         Desert::Editor::KeyBonePose( m_Transaction, &m_Animator, &m_Clip, kChild, Animation::FrameNumber{ 10 } );
    ASSERT_TRUE( keyed.IsSuccess() );
    ASSERT_EQ( keyed.GetValue(), 1U );
    EXPECT_EQ( Desert::Editor::DropPoseRecordsFor( &m_Animator ), 1U );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1U );
    CommandHistory::Get().DropFor( &m_Clip );
    EXPECT_TRUE( CommandHistory::Get().UndoStack().empty() );
}

TEST( UIAnimationUndo, ABindingAndATrackAddedToAUIAnimationAreOneStepAndUndoResetsThePlayer )
{
    CommandHistory::Get().Clear();
    Desert::ECS::UIAnimData animation;
    animation.Sequence.End = Animation::FrameNumber{ 1000 };
    animation.Playback.emplace( animation.Sequence.TickRate, animation.Sequence.Start, animation.Sequence.End );
    const Timeline::Sequence before = animation.Sequence;

    SequenceEditTransaction transaction;
    {
        ScopedSequenceEdit edit( transaction, OwnerOf( &animation ) );
        Timeline::Binding  binding;
        binding.Guid    = Timeline::BindingGuid::Generate();
        binding.Kind    = Timeline::BindingKind::Widget;
        binding.Locator = "1234";
        animation.Sequence.Bindings.push_back( binding );
        Timeline::Track track;
        track.Binding  = binding.Guid;
        track.Property = "Opacity";
        animation.Sequence.Tracks.push_back( track );
    }
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1U );
    ASSERT_NE( Top(), nullptr );
    EXPECT_TRUE( Top()->CarriesHeader() );
    EXPECT_EQ( Top()->EditedObject(), &animation );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( animation.Sequence, before ) );
    EXPECT_FALSE( animation.Playback.has_value() ) << "a restored range must re-create the player";
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_EQ( animation.Sequence.Tracks.size(), 1U );
    CommandHistory::Get().DropVolatile();
    EXPECT_TRUE( CommandHistory::Get().RedoStack().empty() && CommandHistory::Get().UndoStack().empty() );
}

TEST( SequenceCensus, EveryStoredFieldOfATrackIsCompared )
{
    Timeline::Track a;
    a.Property        = "x";
    Timeline::Track b = a;
    EXPECT_TRUE( SameStoredValue( a, b ) );
    b.Muted = true;
    EXPECT_FALSE( SameStoredValue( a, b ) );
    b = a;
    Timeline::Section section;
    section.Content = Timeline::Channel{ Timeline::FloatChannel{} };
    a.Sections.push_back( section );
    section.Content = Timeline::Channel{ Timeline::BoolChannel{} };
    b.Sections.push_back( section );
    EXPECT_FALSE( SameStoredValue( a, b ) ) << "the channel alternative is part of the value";
}

// ── THE CONTROL-RIG DRAG (A33) ───────────────────────────────────────────────────────────────────────
//
// A control's authored pose lives in its ControlHierarchy, not in a Sequence, so its undo entry is
// ControlPoseCommand. Measured both ways: the gesture alone leaves 0 entries and is proved to have CHANGED
// something; the recorded gesture leaves 1 that restores it by value.

namespace
{
    namespace DA = Desert::Animation;

    DA::ControlHierarchy MakeRig( uint32_t& control )
    {
        DA::ControlHierarchy rig;
        DA::ControlElement   hand;
        hand.Name      = "hand_ctrl";
        hand.ShapeName = "CircleXY";
        hand.Parents.push_back( DA::ControlSpace{ DA::ControlSpaceKind::Component, 0, 1.0F } );
        const auto added = rig.Add( hand );
        control          = added.IsSuccess() ? added.GetValue() : DA::ControlHierarchy::INVALID;
        return rig;
    }

    DA::BoneTransform Displaced()
    {
        DA::BoneTransform pose;
        pose.Translation = glm::vec3( 12.0F, -3.5F, 40.0F );
        pose.Rotation    = glm::quat( glm::vec3( 0.0F, 0.4F, 0.0F ) );
        pose.Scale       = glm::vec3( 1.0F );
        return pose;
    }

    DA::BoneTransform AtX( float x )
    {
        DA::BoneTransform pose;
        pose.Translation = glm::vec3( x, 0.0F, 0.0F );
        return pose;
    }

    /// Keys of one part of @p name's Transform track, summed over its sections (X component: every key of a
    /// part is written to all of its components by SetTransformKey).
    size_t PartKeysOf( const DA::AnimationClip& clip, const char* name, DA::TrackChannel part )
    {
        const Timeline::Track* track = DA::FindBoneTrack( clip.Sequence, name );
        if ( track == nullptr )
        {
            return 0;
        }
        size_t keys = 0;
        for ( const Timeline::Section& section : track->Sections )
        {
            const auto* channel = std::get_if<Timeline::Channel>( &section.Content );
            const auto* transform =
                 channel != nullptr ? std::get_if<Timeline::TransformChannel>( channel ) : nullptr;
            if ( transform == nullptr )
            {
                continue;
            }
            switch ( part )
            {
                case DA::TrackChannel::Position: keys += transform->Translation.X.Keys.size(); break;
                case DA::TrackChannel::Rotation: keys += transform->Rotation.X.Keys.size(); break;
                case DA::TrackChannel::Scale: keys += transform->Scale.X.Keys.size(); break;
            }
        }
        return keys;
    }
} // namespace

TEST( ControlDragUndo, TheSameDragIsZeroEntriesUnrecordedAndOneRecorded )
{
    CommandHistory::Get().Clear();
    {
        uint32_t control = 0;
        auto     rig     = MakeRig( control );
        ASSERT_NE( control, DA::ControlHierarchy::INVALID );
        const DA::BoneTransform before = rig.Get( control ).Pose;
        ASSERT_TRUE( rig.SetPose( control, Displaced() ).IsSuccess() );
        EXPECT_FALSE( SameStoredValue( before, rig.Get( control ).Pose ) ) << "the drag really moved the control";
        EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U );
        EXPECT_FALSE( CommandHistory::Get().Undo() );
    }
    CommandHistory::Get().Clear();
    {
        uint32_t control = 0;
        auto     rig     = MakeRig( control );
        ASSERT_NE( control, DA::ControlHierarchy::INVALID );
        const DA::BoneTransform before = rig.Get( control ).Pose;
        ASSERT_TRUE( rig.SetPose( control, Displaced() ).IsSuccess() );
        const auto recorded = Desert::Editor::RecordControlDrag( &rig, control, before );
        ASSERT_TRUE( recorded.IsSuccess() );
        EXPECT_EQ( recorded.GetValue(), 1U );
        ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1U );

        const DA::BoneTransform after = rig.Get( control ).Pose;
        ASSERT_TRUE( CommandHistory::Get().Undo() );
        EXPECT_TRUE( SameStoredValue( before, rig.Get( control ).Pose ) );
        ASSERT_TRUE( CommandHistory::Get().Redo() );
        EXPECT_TRUE( SameStoredValue( after, rig.Get( control ).Pose ) );
    }
    CommandHistory::Get().Clear();
}

TEST( ControlDragUndo, AGrabThatMovedNothingIsNotAnUndoStep )
{
    CommandHistory::Get().Clear();
    uint32_t control = 0;
    auto     rig     = MakeRig( control );
    ASSERT_NE( control, DA::ControlHierarchy::INVALID );
    const auto recorded = Desert::Editor::RecordControlDrag( &rig, control, rig.Get( control ).Pose );
    ASSERT_TRUE( recorded.IsSuccess() );
    EXPECT_EQ( recorded.GetValue(), 0U );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U );
    CommandHistory::Get().Clear();
}

TEST( ControlDragUndo, TheEntryIsVolatileAndNamesAControlThatMustStillExist )
{
    CommandHistory::Get().Clear();
    uint32_t control = 0;
    auto     rig     = MakeRig( control );
    ASSERT_NE( control, DA::ControlHierarchy::INVALID );
    const DA::BoneTransform before = rig.Get( control ).Pose;
    ASSERT_TRUE( rig.SetPose( control, Displaced() ).IsSuccess() );
    ASSERT_TRUE( Desert::Editor::RecordControlDrag( &rig, control, before ).IsSuccess() );
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1U );
    EXPECT_TRUE( CommandHistory::Get().UndoStack().back()->IsVolatile() );
    CommandHistory::Get().DropVolatile();
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U );

    // An entry holding an out-of-range index would crash the first Ctrl+Z after it: refused, not stored.
    EXPECT_FALSE( Desert::Editor::RecordControlDrag( &rig, 99U, before ).IsSuccess() );
    EXPECT_FALSE( Desert::Editor::RecordControlDrag( nullptr, control, before ).IsSuccess() );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U );
    CommandHistory::Get().Clear();
}

// ── THE PALETTE'S TURN REACHES THE BONE, AND THE GIZMO'S GESTURE IS ONE STEP (ANV2a) ────────────────────

namespace
{
    // 1 unit = 1 cm. Root -> Arm, both binds rotated so the bone's local space differs from component space.
    DA::Skeleton MakeTwoBoneArm()
    {
        std::vector<DA::BoneInfo> bones( 2 );
        bones[0].Name = "Root";
        bones[0].LocalBindTransform =
             glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 100.0F, 0.0F ) ) *
             glm::rotate( glm::mat4( 1.0F ), glm::radians( 20.0F ), glm::vec3( 0, 0, 1 ) );
        bones[1].Name         = "Arm";
        bones[1].ParentBoneID = 0U;
        bones[1].LocalBindTransform =
             glm::translate( glm::mat4( 1.0F ), glm::vec3( 30.0F, 0.0F, 0.0F ) ) *
             glm::rotate( glm::mat4( 1.0F ), glm::radians( -10.0F ), glm::vec3( 1, 0, 0 ) );
        DA::Skeleton skeleton( std::move( bones ) );
        skeleton.RecomputeOffsetMatrices();
        return skeleton;
    }

    // The Arm bone's local rotation after one evaluation of the stage over the bind pose.
    glm::quat ArmAfterRig( DA::ControlRigStage& stage, const DA::Skeleton& skeleton )
    {
        auto bind = DA::LocalPose::FromBindPose( skeleton );
        EXPECT_TRUE( bind.IsSuccess() );
        DA::LocalPose     pose = bind.GetValue();
        DA::ComponentPose component( skeleton, pose );
        const auto        evaluated = stage.Evaluate( skeleton, pose, component );
        EXPECT_TRUE( evaluated.IsSuccess() ) << evaluated.GetError();
        return pose[1].Rotation;
    }
} // namespace

TEST( ControlRotateUndo, RotatingTheSelectedControlTurnsItsDrivenBoneAndOneUndoTurnsItBack )
{
    CommandHistory::Get().Clear();
    const DA::Skeleton skeleton = MakeTwoBoneArm();

    DA::ControlRigStage stage;
    DA::ControlElement  elbow;
    elbow.Name             = "Elbow_CTRL";
    elbow.ShapeName        = "CircleXY";
    elbow.Pose.Translation = glm::vec3( 25.0F, 110.0F, 4.0F );
    elbow.Pose.Rotation    = glm::quat( glm::vec3( 0.2F, 0.0F, 0.3F ) );
    elbow.Parents.push_back( DA::ControlSpace{ DA::ControlSpaceKind::Component, 0, 1.0F } );
    const auto added = stage.GetHierarchy().Add( elbow );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    const uint32_t control = added.GetValue();
    const auto     drives  = stage.SetDrives( skeleton, { DA::ControlBoneDrive{ control, 1U } } );
    ASSERT_TRUE( drives.IsSuccess() ) << drives.GetError();

    const glm::quat before = ArmAfterRig( stage, skeleton );
    const auto      turned = Desert::Editor::RotateControlRecorded( &stage.GetHierarchy(), control, 2, 45.0F );
    ASSERT_TRUE( turned.IsSuccess() ) << turned.GetError();
    EXPECT_EQ( turned.GetValue(), 1U );
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1U ) << "one turn must be exactly one undo step";

    // The control is in component space and the bone's local is parent^-1 * control, so before^-1 * after is
    // the control's own local Z turn whatever the parent is.
    const glm::quat after = ArmAfterRig( stage, skeleton );
    EXPECT_NEAR( glm::degrees( glm::angle( glm::inverse( before ) * after ) ), 45.0F, 0.05F );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    const glm::quat undone = ArmAfterRig( stage, skeleton );
    EXPECT_LT( glm::degrees( glm::angle( glm::inverse( before ) * undone ) ), 0.05F )
         << "undo must put the BONE back, not only the control";

    EXPECT_FALSE( Desert::Editor::RotateControlRecorded( &stage.GetHierarchy(), 7U, 2, 45.0F ).IsSuccess() );
    EXPECT_FALSE( Desert::Editor::RotateControlRecorded( nullptr, control, 2, 45.0F ).IsSuccess() );
    CommandHistory::Get().Clear();
}

TEST( ControlGizmoGestureUndo, APressDragReleaseIsOneEntryHoweverManyFramesItMoved )
{
    CommandHistory::Get().Clear();
    uint32_t control = 0;
    auto     rig     = MakeRig( control );
    ASSERT_NE( control, DA::ControlHierarchy::INVALID );
    const DA::BoneTransform grabbed = rig.Get( control ).Pose;

    Desert::Editor::ControlGizmoGesture gesture;
    for ( int frame = 0; frame < 40; ++frame ) // forty held frames, each writing the control as Manipulate does
    {
        const auto step = gesture.Step( &rig, control, true );
        ASSERT_TRUE( step.IsSuccess() ) << step.GetError();
        EXPECT_EQ( step.GetValue(), 0U );
        DA::BoneTransform moved = rig.Get( control ).Pose;
        moved.Translation += glm::vec3( 0.5F, 0.0F, 0.0F );
        ASSERT_TRUE( rig.SetPose( control, moved ).IsSuccess() );
    }
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U ) << "nothing is recorded while the gizmo is held";

    const auto released = gesture.Step( &rig, control, false );
    ASSERT_TRUE( released.IsSuccess() ) << released.GetError();
    EXPECT_EQ( released.GetValue(), 1U );
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1U );
    EXPECT_FALSE( gesture.Active() );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( grabbed, rig.Get( control ).Pose ) ) << "one undo returns the whole gesture";

    CommandHistory::Get().Clear();
    ASSERT_TRUE( gesture.Step( &rig, control, true ).IsSuccess() );
    const auto still = gesture.Step( &rig, control, false );
    ASSERT_TRUE( still.IsSuccess() );
    EXPECT_EQ( still.GetValue(), 0U ) << "a press and release that moved nothing is not an undo step";
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U );
    CommandHistory::Get().Clear();
}

TEST( ControlGizmoGestureUndo, AGestureWhoseControlChangedBeforeTheReleaseIsAbandonedNotRecorded )
{
    CommandHistory::Get().Clear();
    uint32_t control = 0;
    auto     rig     = MakeRig( control );
    ASSERT_NE( control, DA::ControlHierarchy::INVALID );
    Desert::Editor::ControlGizmoGesture gesture;
    ASSERT_TRUE( gesture.Step( &rig, control, true ).IsSuccess() );
    ASSERT_TRUE( rig.SetPose( control, Displaced() ).IsSuccess() );
    EXPECT_FALSE( gesture.Step( &rig, control + 1U, false ).IsSuccess() );
    EXPECT_FALSE( gesture.Active() );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U );
    CommandHistory::Get().Clear();
}

// ── ANV2b: the Sequencer's control rows (Key (S), auto-key, scrub, key drag/delete) ─────────────────────

TEST_F( ClipEditUndo, KeyingTwoTicksThenScrubbingPutsTheControlBetweenThemAndEachKeyIsOneUndo )
{
    uint32_t control   = 0;
    auto     hierarchy = MakeRig( control );
    ASSERT_NE( control, DA::ControlHierarchy::INVALID );
    const std::array<uint32_t, 1> selected = { control };

    ASSERT_TRUE( hierarchy.SetPose( control, AtX( 0.0F ) ).IsSuccess() );
    m_Tick     = DA::FrameNumber{ 0 };
    auto first = Desert::Editor::KeyControlsRecorded( m_Transaction, &m_Animator, m_Keyer, Target( &hierarchy ),
                                                      selected );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    EXPECT_EQ( first.GetValue(), 1U );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1U ) << "one Key press is one undo entry";

    ASSERT_TRUE( hierarchy.SetPose( control, AtX( 20.0F ) ).IsSuccess() );
    m_Tick      = DA::FrameNumber{ kDisplayFrameTicks * 20 };
    auto second = Desert::Editor::KeyControlsRecorded( m_Transaction, &m_Animator, m_Keyer, Target( &hierarchy ),
                                                       selected );
    ASSERT_TRUE( second.IsSuccess() ) << second.GetError();
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 2U );
    EXPECT_EQ( PartKeysOf( m_Clip, "hand_ctrl", DA::TrackChannel::Position ), 2U );

    // Scrub to the middle: the control is interpolated, not left where the pointer put it.
    ASSERT_TRUE( hierarchy.SetPose( control, AtX( -77.0F ) ).IsSuccess() );
    m_Tick = DA::FrameNumber{ kDisplayFrameTicks * 10 };
    DA::ControlKeyer playback;
    const auto       applied = DA::ApplyClipToControls( Target( &hierarchy ), playback );
    ASSERT_TRUE( applied.IsSuccess() ) << applied.GetError();
    EXPECT_EQ( applied.GetValue(), 1U );
    EXPECT_NEAR( hierarchy.Get( control ).Pose.Translation.x, 10.0F, 0.01F );
    EXPECT_EQ( PartKeysOf( m_Clip, "hand_ctrl", DA::TrackChannel::Position ), 2U ) << "playback must key nothing";

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( PartKeysOf( m_Clip, "hand_ctrl", DA::TrackChannel::Position ), 1U ) << "undo removes the second key";
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( PartKeysOf( m_Clip, "hand_ctrl", DA::TrackChannel::Position ), 0U );
    EXPECT_EQ( DA::FindBoneTrack( m_Clip.Sequence, "hand_ctrl" ), nullptr ) << "the track the key created goes too";

    const std::vector<uint32_t> none;
    EXPECT_FALSE(
         Desert::Editor::KeyControlsRecorded( m_Transaction, &m_Animator, m_Keyer, Target( &hierarchy ), none )
              .IsSuccess() );
}

TEST_F( ClipEditUndo, AutoKeyWritesExactlyOneKeyAndOneEntryPerControlGesture )
{
    AutoKey( DA::AutoChangeMode::All );
    uint32_t control   = 0;
    auto     hierarchy = MakeRig( control );
    ASSERT_NE( control, DA::ControlHierarchy::INVALID );

    Desert::Editor::ControlAutoKey autoKey;
    uint32_t                       entries = 0;
    const auto                     step    = [&]( bool held )
    {
        const auto stepped = autoKey.Step( m_Transaction, &m_Animator, m_Keyer, Target( &hierarchy ), control, held );
        EXPECT_TRUE( stepped.IsSuccess() ) << ( stepped.IsSuccess() ? "" : stepped.GetError() );
        entries += stepped.IsSuccess() ? stepped.GetValue() : 0U;
    };

    step( false );
    for ( int frame = 1; frame <= 30; ++frame )
    {
        ASSERT_TRUE( hierarchy.SetPose( control, AtX( static_cast<float>( frame ) ) ).IsSuccess() );
        step( true );
        EXPECT_EQ( PartKeysOf( m_Clip, "hand_ctrl", DA::TrackChannel::Position ), 0U )
             << "nothing is keyed while the gesture is held";
    }
    step( false );
    EXPECT_EQ( entries, 1U );
    EXPECT_EQ( PartKeysOf( m_Clip, "hand_ctrl", DA::TrackChannel::Position ), 1U ) << "thirty frames are ONE key";
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1U );

    for ( int frame = 0; frame < 10; ++frame ) // a press that moves nothing keys nothing
    {
        step( true );
    }
    step( false );
    EXPECT_EQ( entries, 1U );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1U );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( PartKeysOf( m_Clip, "hand_ctrl", DA::TrackChannel::Position ), 0U );
}

// THE ONE ENTRY POINT: a command that changes a control without the gizmo bit (the palette's rotate, a nudge,
// the Control Rig panel's pose field) records through RecordControlDrag, and that alone makes the auto-keyer
// key it — one key, and ONE undo entry for the edit and its key together.
TEST_F( ClipEditUndo, AutoKeyKeysACommandEditWithoutTheGizmoAndOneUndoTakesBackPoseAndKey )
{
    AutoKey( DA::AutoChangeMode::All );
    uint32_t control   = 0;
    auto     hierarchy = MakeRig( control );
    ASSERT_NE( control, DA::ControlHierarchy::INVALID );

    Desert::Editor::ControlAutoKey autoKey;
    uint32_t                       entries = 0;
    const auto                     step    = [&]()
    {
        const auto stepped =
             autoKey.Step( m_Transaction, &m_Animator, m_Keyer, Target( &hierarchy ), control, false );
        EXPECT_TRUE( stepped.IsSuccess() ) << ( stepped.IsSuccess() ? "" : stepped.GetError() );
        entries += stepped.IsSuccess() ? stepped.GetValue() : 0U;
    };
    const auto rotationKeys = [&]() { return PartKeysOf( m_Clip, "hand_ctrl", DA::TrackChannel::Rotation ); };

    // An edit recorded BEFORE this keyer existed is not a new one.
    const DA::BoneTransform original = hierarchy.Get( control ).Pose;
    ASSERT_TRUE( Desert::Editor::RotateControlRecorded( &hierarchy, control, 2, 45.0F ).IsSuccess() );
    CommandHistory::Get().Clear();
    step();
    step();
    EXPECT_EQ( rotationKeys(), 0U ) << "an old edit must not key a new Sequencer";

    // A pose change that no gesture made (the playhead writing the clip back) keys nothing.
    ASSERT_TRUE( hierarchy.SetPose( control, original ).IsSuccess() );
    step();
    step();
    EXPECT_EQ( rotationKeys(), 0U ) << "a change outside a gesture is not a key";
    EXPECT_EQ( entries, 0U );

    const DA::BoneTransform before = hierarchy.Get( control ).Pose;
    const auto              turned = Desert::Editor::RotateControlRecorded( &hierarchy, control, 2, 45.0F );
    ASSERT_TRUE( turned.IsSuccess() ) << turned.GetError();
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1U );
    step(); // the edit is seen: the gesture is held for this frame
    EXPECT_EQ( rotationKeys(), 0U );
    step(); // and released on the next: the key
    step();
    EXPECT_EQ( entries, 1U );
    EXPECT_EQ( rotationKeys(), 1U ) << "the rotate command is one key";
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1U ) << "the edit and its key are ONE undo entry";

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( rotationKeys(), 0U ) << "undo removes the key";
    EXPECT_TRUE( SameStoredValue( hierarchy.Get( control ).Pose, before ) ) << "and the same undo puts the control back";
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_EQ( rotationKeys(), 1U );

    // Auto Key off: the command still records its pose entry, and keys nothing.
    m_Keyer.SetModes( DA::KeyingModes{} );
    ASSERT_TRUE( Desert::Editor::RotateControlRecorded( &hierarchy, control, 2, 45.0F ).IsSuccess() );
    step();
    step();
    EXPECT_EQ( entries, 1U );
    EXPECT_EQ( rotationKeys(), 1U );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 2U );
}

// A control row's key is ONE key in the ruler and three parts in the track: dragging or deleting it moves all
// three, and the whole interaction is one undo step (the Sequencer wraps it in one ScopedSequenceEdit).
TEST_F( ClipEditUndo, AControlRowKeyMovesAndDeletesAllThreeChannelsTogetherAsOneUndoStep )
{
    constexpr std::array<DA::TrackChannel, 3> kParts = { DA::TrackChannel::Position, DA::TrackChannel::Rotation,
                                                         DA::TrackChannel::Scale };
    const DA::FrameNumber first{ 0 };
    const DA::FrameNumber last{ kDisplayFrameTicks * 20 };
    const DA::FrameNumber moved{ kDisplayFrameTicks * 5 };
    ASSERT_TRUE( DA::SetBoneKey( m_Clip.Sequence, "hand_ctrl", first, AtX( 0.0F ) ).IsSuccess() );
    ASSERT_TRUE( DA::SetBoneKey( m_Clip.Sequence, "hand_ctrl", last, AtX( 20.0F ) ).IsSuccess() );
    const Timeline::Sequence before = m_Clip.Sequence;

    for ( const DA::TrackChannel part : kParts )
    {
        EXPECT_FALSE( DA::MoveBoneKey( m_Clip.Sequence, "hand_ctrl", part, first, last ).IsSuccess() )
             << "landing on a key would merge two keys into one";
    }
    EXPECT_TRUE( SameStoredValue( m_Clip.Sequence, before ) ) << "a refused move changes nothing";

    {
        ScopedSequenceEdit edit( m_Transaction, OwnerOf( &m_Clip ) );
        for ( const DA::TrackChannel part : kParts )
        {
            const auto result = DA::MoveBoneKey( m_Clip.Sequence, "hand_ctrl", part, first, moved );
            ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
        }
    }
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1U ) << "one drag of a row key is one undo step";
    const Timeline::Sequence afterMove = m_Clip.Sequence;
    {
        ScopedSequenceEdit edit( m_Transaction, OwnerOf( &m_Clip ) );
        for ( const DA::TrackChannel part : kParts )
        {
            const auto result = DA::RemoveBoneKey( m_Clip.Sequence, "hand_ctrl", part, moved );
            ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
        }
    }
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 2U );
    for ( const DA::TrackChannel part : kParts )
    {
        EXPECT_EQ( PartKeysOf( m_Clip, "hand_ctrl", part ), 1U );
        EXPECT_FALSE( DA::RemoveBoneKey( m_Clip.Sequence, "hand_ctrl", part, DA::FrameNumber{ 1 } ).IsSuccess() );
    }

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( m_Clip.Sequence, afterMove ) );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( m_Clip.Sequence, before ) );
}

// ── THE ANIMATION EDITOR'S BONE GIZMO (ANV4e): one entry per press-drag-release ───────────────────────────

namespace
{
    /// `AnimationEditorDocument::DrawBoneGizmo` per frame: the gesture steps on ImGuizmo's held bit, THEN the
    /// frame's write lands through the authoring pose.
    uint32_t GizmoFrame( SequenceEditTransaction& transaction, DA::Animator& animator, DA::AnimationClip& clip,
                         Desert::Editor::BoneGizmoGesture& gesture, bool held, std::optional<float> y,
                         int& posingCalls )
    {
        const auto stepped = gesture.Step(
             transaction, held,
             [&]()
             {
                 ++posingCalls;
                 return &animator;
             },
             &clip );
        EXPECT_TRUE( stepped.IsSuccess() ) << ( stepped.IsSuccess() ? "" : stepped.GetError() );
        if ( y.has_value() && gesture.Active() )
        {
            DA::LocalPose edited = animator.GetAuthoringPose();
            edited[kChild]       = At( *y );
            EXPECT_TRUE( animator.SetAuthoringPose( edited ).IsSuccess() );
            animator.ApplyLocalPose();
        }
        return stepped.IsSuccess() ? stepped.GetValue() : 0;
    }

    float Lifted( int frame )
    {
        return 1.0F + 0.02F * static_cast<float>( frame );
    }
} // namespace

TEST_F( ClipEditUndo, ABoneGizmoDragOfFortyFramesIsOneUndoStepAndUndoReturnsTheBone )
{
    Desert::Editor::BoneGizmoGesture gesture;
    int                              posingCalls = 0;
    const DA::LocalPose              poseBefore  = m_Animator.GetAuthoringPose();

    uint32_t pushed = GizmoFrame( m_Transaction, m_Animator, m_Clip, gesture, false, std::nullopt, posingCalls );
    for ( int i = 1; i <= 40; ++i )
    {
        pushed += GizmoFrame( m_Transaction, m_Animator, m_Clip, gesture, true, Lifted( i ), posingCalls );
    }
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U ) << "nothing may be recorded while the drag is held";
    pushed += GizmoFrame( m_Transaction, m_Animator, m_Clip, gesture, false, std::nullopt, posingCalls );

    EXPECT_EQ( pushed, 1U ) << "forty held frames must push one entry, not forty";
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1U );
    EXPECT_EQ( posingCalls, 1 ) << "the preview is switched into posing on the press only";
    EXPECT_FALSE( gesture.Active() );
    EXPECT_FALSE( m_Transaction.Open() );
    ASSERT_FALSE( SameStoredValue( poseBefore, m_Animator.GetAuthoringPose() ) ) << "the drag moved nothing";

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( poseBefore, m_Animator.GetAuthoringPose() ) )
         << "undo did not return the bone to where the press found it";
}

TEST_F( ClipEditUndo, ABoneGizmoPressThatMovesNothingIsNoUndoStep )
{
    Desert::Editor::BoneGizmoGesture gesture;
    int                              posingCalls = 0;
    uint32_t pushed = GizmoFrame( m_Transaction, m_Animator, m_Clip, gesture, false, std::nullopt, posingCalls );
    for ( int i = 1; i <= 10; ++i )
    {
        pushed += GizmoFrame( m_Transaction, m_Animator, m_Clip, gesture, true, std::nullopt, posingCalls );
    }
    pushed += GizmoFrame( m_Transaction, m_Animator, m_Clip, gesture, false, std::nullopt, posingCalls );

    EXPECT_EQ( posingCalls, 1 ) << "the press happened, so the 0 below is not vacuous";
    EXPECT_EQ( pushed, 0U );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0U );
    EXPECT_FALSE( m_Transaction.Open() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
