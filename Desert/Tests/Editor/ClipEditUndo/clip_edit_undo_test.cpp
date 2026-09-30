// ONE UNDO PATH FOR ANIMATION DATA: SequenceEditTransaction / SequenceEditCommand over a clip's and a UI
// animation's Timeline::Sequence (Editor/Core/Commands/SequenceEdit.hpp).

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/PoseEditTransaction.hpp>
#include <Editor/Core/Commands/SequenceEdit.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Engine/ECS/Components.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <memory>
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

        Animation::Skeleton      m_Skeleton = MakeChain();
        Animation::Animator      m_Animator{ m_Skeleton };
        Animation::AnimationClip m_Clip = MakeClip();
        SequenceEditTransaction  m_Transaction;
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
    ASSERT_EQ(
         Desert::Editor::KeyBonePose( m_Transaction, &m_Animator, &m_Clip, kChild, Animation::FrameNumber{ 10 } )
              .GetValue(),
         1U );
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
    animation.Playback.emplace();
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
