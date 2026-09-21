// HOW MANY UNDO STEPS IS ONE DRAG, AND WHAT DOES THE STEP PUT BACK.
//
// The headline number is measured in both directions, by running the same drag twice:
//
//   * WITHOUT the transaction — the keyer alone, which is exactly what `SequencerPanel` did before this
//     change — the undo stack stays at 0 entries. That is the positive control, and without it "the
//     stack has one entry" would be satisfied by a stack that always has one.
//   * WITH the transaction, the same sixty frames leave 1.
//
// The round trip is asserted BY VALUE, not by eye: `SameStoredValue` compares the stored representation
// of every bone of the authoring pose and every key of every track, reserved fields included. And the
// scenario is guarded against being vacuous the way A22's was — every round-trip test first asserts that
// the drag actually CHANGED the thing it then undoes, because a keyer that wrote nothing would pass
// "undo restored it" perfectly.
//
// THE FRAME ORDER IS THE CONTENT OF `Frame()` BELOW, and it is the panel's order: the manipulator writes
// the pose first (it runs in the viewport, before the Sequencer), then the keyer observes, then the pose
// is reloaded from the clip if keys landed, and only then does the transaction close. A transaction that
// closed earlier would record an "after" missing its own keys.

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/PoseEditTransaction.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlKeyer.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

using Desert::Animation::AnimationClip;
using Desert::Animation::Animator;
using Desert::Animation::AutoChangeMode;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTrack;
using Desert::Animation::BoneTransform;
using Desert::Animation::ControlKeyer;
using Desert::Animation::ControlKeyTarget;
using Desert::Animation::DEFAULT_DISPLAY_RATE;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::KeyGroupMode;
using Desert::Animation::KeyingModes;
using Desert::Animation::KeySubject;
using Desert::Animation::KeySubjectKind;
using Desert::Animation::LocalPose;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::Skeleton;
using Desert::Editor::ClipPoseCommand;
using Desert::Editor::CommandHistory;
using Desert::Editor::PoseEditTransaction;
using Desert::Editor::SameStoredValue;
using Desert::Editor::ScopedPoseEdit;

namespace
{
    constexpr uint32_t kChild = 1;
    /// One display frame at 30 fps on the project's 24000-tick grid. Written as the division rather than
    /// as 800 so that a change to either rate moves it.
    constexpr int32_t kDisplayFrameTicks = PROJECT_TICK_RATE.Numerator / DEFAULT_DISPLAY_RATE.Numerator;

    Skeleton MakeChain()
    {
        std::vector<BoneInfo> bones( 2 );
        bones[0].Name               = "root";
        bones[0].ParentBoneID       = std::nullopt;
        bones[0].LocalBindTransform = glm::mat4( 1.0f );

        bones[1].Name               = "child";
        bones[1].ParentBoneID       = 0u;
        bones[1].LocalBindTransform = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );

        Skeleton skeleton( std::move( bones ) );
        skeleton.RecomputeOffsetMatrices();
        return skeleton;
    }

    /// A clip that already has keys AT BOTH ENDS of the child's position channel. The endpoints are the
    /// whole point: `SetTransformKey` refreshes the WHOLE track's auto tangents after an upsert, so a key
    /// written between them changes THEIR slopes too. An undo built as "delete the key I added" would
    /// leave those two keys holding tangents computed against a key that no longer exists, and every
    /// assertion about the key count would still pass.
    AnimationClip MakeClipWithEndpoints()
    {
        AnimationClip clip;
        clip.AnimationName = "take01";
        clip.DurationTicks = FrameNumber{ PROJECT_TICK_RATE.Numerator };
        clip.TickRate      = PROJECT_TICK_RATE;
        clip.DisplayRate   = DEFAULT_DISPLAY_RATE;

        BoneTrack track;
        track.BoneName = "child";
        track.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 0.0f, 1.0f, 0.0f ) } );
        track.PositionKeys.push_back(
             { FrameNumber{ PROJECT_TICK_RATE.Numerator }, glm::vec3( 0.0f, 3.0f, 0.0f ) } );
        Desert::Animation::RefreshTangents( track, clip.TickRate );
        clip.Tracks.push_back( std::move( track ) );
        return clip;
    }

    /// FOUR keys, so that the two on either side of the tick we key at are INTERIOR ones. An endpoint's
    /// auto tangents are flat by rule (`AutoSetTangents`: a curve that leaves its last key with a slope
    /// overshoots past the end of the clip), so a three-key track cannot show the neighbour effect at all
    /// -- which is exactly the degenerate scenario §8.4 warns about, and this factory exists because the
    /// first version of the tangent test used the clip above and passed for that reason.
    AnimationClip MakeClipWithInteriorKeys()
    {
        AnimationClip clip;
        clip.AnimationName = "take03";
        clip.DurationTicks = FrameNumber{ PROJECT_TICK_RATE.Numerator };
        clip.TickRate      = PROJECT_TICK_RATE;
        clip.DisplayRate   = DEFAULT_DISPLAY_RATE;

        BoneTrack track;
        track.BoneName = "child";
        // Ticks on the 30 fps display grid (800 ticks each), values chosen so consecutive secants differ:
        // a straight line would give every interior key the same slope and hide the effect again.
        track.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 0.0f, 0.0f, 0.0f ) } );
        track.PositionKeys.push_back( { FrameNumber{ kDisplayFrameTicks * 10 }, glm::vec3( 0.0f, 1.0f, 0.0f ) } );
        track.PositionKeys.push_back( { FrameNumber{ kDisplayFrameTicks * 20 }, glm::vec3( 0.0f, 5.0f, 0.0f ) } );
        track.PositionKeys.push_back( { FrameNumber{ kDisplayFrameTicks * 30 }, glm::vec3( 0.0f, 6.0f, 0.0f ) } );
        Desert::Animation::RefreshTangents( track, clip.TickRate );
        clip.Tracks.push_back( std::move( track ) );
        return clip;
    }

    /// A clip with NO track for the child, to watch keying create one (and undo take it away).
    AnimationClip MakeClipWithNoChildTrack()
    {
        AnimationClip clip;
        clip.AnimationName = "take02";
        clip.DurationTicks = FrameNumber{ PROJECT_TICK_RATE.Numerator };
        clip.TickRate      = PROJECT_TICK_RATE;
        clip.DisplayRate   = DEFAULT_DISPLAY_RATE;
        return clip;
    }

    bool SameTracks( const std::vector<BoneTrack>& a, const std::vector<BoneTrack>& b )
    {
        if ( a.size() != b.size() )
        {
            return false;
        }
        for ( size_t i = 0; i < a.size(); ++i )
        {
            if ( !SameStoredValue( a[i], b[i] ) )
            {
                return false;
            }
        }
        return true;
    }

    size_t KeyCount( const AnimationClip& clip, const char* bone )
    {
        for ( const BoneTrack& track : clip.Tracks )
        {
            if ( track.BoneName == bone )
            {
                return track.PositionKeys.size() + track.RotationKeys.size() + track.ScaleKeys.size();
            }
        }
        return 0;
    }

    /// The panel, reduced to the four things it does per frame in the pose-authoring branch. Everything
    /// here is a call the panel makes, in the order it makes it.
    class Rig
    {
    public:
        Rig( AnimationClip clip, AutoChangeMode change )
             : m_Skeleton( MakeChain() ), m_Animator( m_Skeleton ), m_Clip( std::move( clip ) )
        {
            KeyingModes modes;
            modes.AutoChange = change;
            modes.KeyGroup   = KeyGroupMode::Subject;
            m_Keyer.SetModes( modes );
            m_Animator.ApplyLocalPose();
            m_RecordLast = m_Animator.GetBoneLocalPose( kChild );
        }

        [[nodiscard]] ControlKeyTarget Target()
        {
            ControlKeyTarget target;
            target.Skeleton     = &m_Skeleton;
            target.Clip         = &m_Clip;
            target.AuthoredPose = &m_Animator.GetAuthoringPose();
            target.Tick         = m_Tick;
            return target;
        }

        void SetTick( int32_t tick )
        {
            m_Tick = FrameNumber{ tick };
        }

        /**
         * @brief One frame. @p write is the manipulator's edit, applied FIRST because the viewport runs
         *        before the Sequencer — which is the whole reason the transaction keeps a baseline.
         * @return undo entries pushed by this frame.
         */
        uint32_t Frame( bool held, std::optional<glm::mat4> write, bool withTransaction = true )
        {
            if ( write.has_value() )
            {
                m_Animator.SetBoneLocalPose( kChild, *write );
                m_Animator.ApplyLocalPose();
            }

            const glm::mat4 current = m_Animator.GetBoneLocalPose( kChild );
            const bool      moved   = ( current != m_RecordLast );

            const auto observed =
                 m_Keyer.Observe( Target(), KeySubject{ KeySubjectKind::Bone, kChild }, held, moved );
            EXPECT_TRUE( observed.IsSuccess() ) << ( observed.IsSuccess() ? "" : observed.GetError() );
            if ( observed.IsSuccess() && observed.GetValue() > 0 )
            {
                // `showKeyedPose` in the panel: the clip moved under the buffer, so reload it.
                m_Animator.SampleClipIntoLocalPose( m_Clip, FrameTime{ m_Tick, 0.0f } );
                m_Animator.ApplyLocalPose();
                m_RecordLast = m_Animator.GetBoneLocalPose( kChild );
            }
            else if ( moved )
            {
                m_RecordLast = current;
            }

            if ( !withTransaction )
            {
                return 0;
            }
            const auto pushed = m_Transaction.Observe( &m_Animator, &m_Clip, held );
            EXPECT_TRUE( pushed.IsSuccess() ) << ( pushed.IsSuccess() ? "" : pushed.GetError() );
            return pushed.IsSuccess() ? pushed.GetValue() : 0;
        }

        /// A whole drag: one idle frame to seed the baseline, @p frames of holding while the bone climbs,
        /// then the release frame. Returns entries pushed across all of them.
        uint32_t Drag( int frames, float perFrame, bool withTransaction = true )
        {
            uint32_t pushed = Frame( false, std::nullopt, withTransaction );
            for ( int i = 1; i <= frames; ++i )
            {
                pushed += Frame( true,
                                 glm::translate( glm::mat4( 1.0f ),
                                                 glm::vec3( 0.0f, 1.0f + perFrame * static_cast<float>( i ),
                                                            0.0f ) ),
                                 withTransaction );
            }
            pushed += Frame( false, std::nullopt, withTransaction );
            return pushed;
        }

        Skeleton            m_Skeleton;
        Animator            m_Animator;
        AnimationClip       m_Clip;
        ControlKeyer        m_Keyer;
        PoseEditTransaction m_Transaction;
        glm::mat4           m_RecordLast = glm::mat4( 1.0f );
        FrameNumber         m_Tick{ kDisplayFrameTicks * 15 };
    };

    class ClipEditUndo : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            CommandHistory::Get().Clear();
        }
        void TearDown() override
        {
            // The history is a process-wide singleton; leaving entries behind would let one test's
            // pointers into a destroyed Animator be reached by the next one's Undo.
            CommandHistory::Get().Clear();
        }
    };
} // namespace

// ── THE HEADLINE NUMBER, MEASURED IN BOTH DIRECTIONS ────────────────────────────────────────────────

TEST_F( ClipEditUndo, ADragWithNoTransactionLeavesNothingToUndo )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::All );
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 0u );

    const uint32_t pushed = rig.Drag( 30, 0.02f, /*withTransaction=*/false );

    EXPECT_EQ( pushed, 0u );
    // The drag really happened: a key landed in the clip, and the stack is still empty. This is the
    // state `SequencerPanel` shipped in — sixty frames of authoring and nothing to take back.
    EXPECT_EQ( KeyCount( rig.m_Clip, "child" ), 3u + 2u ) << "the drag keyed nothing, so the 0 is vacuous";
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0u );
}

TEST_F( ClipEditUndo, OneDragIsExactlyOneUndoStep )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::All );

    const uint32_t pushed = rig.Drag( 30, 0.02f );

    EXPECT_EQ( pushed, 1u ) << "thirty frames of holding must push one entry, not thirty";
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1u );

    // AND THE ENTRY IS NOT EMPTY. One entry holding nothing would satisfy the count above and undo
    // nothing at all, which is the shape §8.4 warns about.
    const auto* entry = dynamic_cast<const ClipPoseCommand*>( CommandHistory::Get().UndoStack().back().get() );
    ASSERT_NE( entry, nullptr );
    EXPECT_EQ( entry->ChangedBones(), 1u );
    EXPECT_GE( entry->ChangedTracks(), 1u );
}

// ── THE ROUND TRIP, BY VALUE ────────────────────────────────────────────────────────────────────────

TEST_F( ClipEditUndo, UndoRestoresBothThePoseAndTheClipByValue )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::All );

    const LocalPose              poseBefore   = rig.m_Animator.GetAuthoringPose();
    const std::vector<BoneTrack> tracksBefore = rig.m_Clip.Tracks;

    ASSERT_EQ( rig.Drag( 30, 0.02f ), 1u );

    // POSITIVE CONTROL FOR THE SCENARIO ITSELF: both halves moved. Without this the two restores below
    // are assertions that nothing changed and then nothing changed back.
    ASSERT_FALSE( SameStoredValue( poseBefore, rig.m_Animator.GetAuthoringPose() ) );
    ASSERT_FALSE( SameTracks( tracksBefore, rig.m_Clip.Tracks ) );

    ASSERT_TRUE( CommandHistory::Get().Undo() );

    EXPECT_TRUE( SameStoredValue( poseBefore, rig.m_Animator.GetAuthoringPose() ) )
         << "the pose came back near, not equal";
    EXPECT_TRUE( SameTracks( tracksBefore, rig.m_Clip.Tracks ) )
         << "the clip came back near, not equal -- the endpoint tangents are the usual survivor";
}

TEST_F( ClipEditUndo, UndoRestoresTheNEIGHBOURSTangents )
{
    // EVERY OTHER ASSERTION IN THIS FILE IS ONLY AS STRONG AS `SameStoredValue`; this one reads the
    // floats. The fields it reads are exactly what an undo built as "delete the key I added" would leave
    // behind: `SetTransformKey` refreshes the WHOLE track's auto tangents after an upsert, so the two
    // keys that were already there are changed by a key written BETWEEN them, and removing that key does
    // not change them back.
    Rig rig( MakeClipWithInteriorKeys(), AutoChangeMode::All );
    rig.SetTick( kDisplayFrameTicks * 15 ); // between keys 1 and 2, so both of them get a new neighbour

    const glm::vec3 leftLeaveBefore   = rig.m_Clip.Tracks[0].PositionKeys[1].LeaveTangent;
    const glm::vec3 rightArriveBefore = rig.m_Clip.Tracks[0].PositionKeys[2].ArriveTangent;
    ASSERT_NE( leftLeaveBefore, glm::vec3( 0.0f ) ) << "an interior key with a flat tangent proves nothing";

    ASSERT_EQ( rig.Drag( 30, 0.02f ), 1u );
    ASSERT_EQ( rig.m_Clip.Tracks[0].PositionKeys.size(), 5u );

    // Positive control: the neighbours really did move. Without it the two restores below are assertions
    // that nothing changed and then nothing changed back -- A22's degenerate scenario, in this shape.
    ASSERT_NE( rig.m_Clip.Tracks[0].PositionKeys[1].LeaveTangent, leftLeaveBefore );
    ASSERT_NE( rig.m_Clip.Tracks[0].PositionKeys[3].ArriveTangent, rightArriveBefore );

    ASSERT_TRUE( CommandHistory::Get().Undo() );

    ASSERT_EQ( rig.m_Clip.Tracks[0].PositionKeys.size(), 4u );
    EXPECT_EQ( rig.m_Clip.Tracks[0].PositionKeys[1].LeaveTangent, leftLeaveBefore );
    EXPECT_EQ( rig.m_Clip.Tracks[0].PositionKeys[2].ArriveTangent, rightArriveBefore );
}

TEST_F( ClipEditUndo, AnEdgeDrivenTransactionIsNotReportedAsExplicit )
{
    // The panel sweeps `OpenExplicitly()` transactions at the top of every frame, because a widget that
    // stopped being drawn never closes its own. A gizmo drag submits no ImGui item, so if it reported
    // itself explicit the sweep would close it on the frame after it opened -- turning the headline
    // "one drag, one step" into "one drag, one step at the very start of it".
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::None );

    EXPECT_EQ( rig.Frame( false, std::nullopt ), 0u );
    EXPECT_EQ( rig.Frame( true, glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 2.0f, 0.0f ) ) ), 0u );
    EXPECT_TRUE( rig.m_Transaction.Open() );
    EXPECT_FALSE( rig.m_Transaction.OpenExplicitly() );

    ASSERT_TRUE( rig.m_Transaction.Begin( &rig.m_Animator, &rig.m_Clip ).IsSuccess() ==
                 false ); // and it still refuses to nest
    EXPECT_EQ( rig.Frame( false, std::nullopt ), 1u );
}

TEST_F( ClipEditUndo, RedoPutsTheSameEditBackByValue )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::All );
    ASSERT_EQ( rig.Drag( 30, 0.02f ), 1u );

    const LocalPose              poseAfter   = rig.m_Animator.GetAuthoringPose();
    const std::vector<BoneTrack> tracksAfter = rig.m_Clip.Tracks;

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    ASSERT_FALSE( SameStoredValue( poseAfter, rig.m_Animator.GetAuthoringPose() ) );
    ASSERT_TRUE( CommandHistory::Get().Redo() );

    EXPECT_TRUE( SameStoredValue( poseAfter, rig.m_Animator.GetAuthoringPose() ) );
    EXPECT_TRUE( SameTracks( tracksAfter, rig.m_Clip.Tracks ) );
}

TEST_F( ClipEditUndo, UndoRemovesATrackKeyingCreated )
{
    Rig rig( MakeClipWithNoChildTrack(), AutoChangeMode::All );
    ASSERT_EQ( rig.m_Clip.Tracks.size(), 0u );

    ASSERT_EQ( rig.Drag( 10, 0.05f ), 1u );
    ASSERT_EQ( rig.m_Clip.Tracks.size(), 1u ) << "keying was supposed to create the child's track";

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    // NOT "an empty track is left behind". A track with a name and no keys is a lane in the Sequencer
    // that the animator never asked for, and it survives a save.
    EXPECT_EQ( rig.m_Clip.Tracks.size(), 0u );
}

// ── THE FRAME-ORDER TRAP THE BASELINE EXISTS FOR ────────────────────────────────────────────────────

TEST_F( ClipEditUndo, TheBeforePoseIsLastFramesEvenWhenTheFirstWriteSharesTheRisingEdge )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::None ); // pose only: this is about the pose half

    const LocalPose poseBefore = rig.m_Animator.GetAuthoringPose();

    // One idle frame to seed the baseline, then a frame that BOTH writes the pose and raises the bit —
    // which is what `GizmoController` does: it calls SetPoseInteraction(true) and SetBoneLocalPose in the
    // same function, and the panel that reads the bit runs afterwards.
    EXPECT_EQ( rig.Frame( false, std::nullopt ), 0u );
    EXPECT_EQ( rig.Frame( true, glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 5.0f, 0.0f ) ) ), 0u );
    EXPECT_EQ( rig.Frame( true, glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 7.0f, 0.0f ) ) ), 0u );
    EXPECT_EQ( rig.Frame( false, std::nullopt ), 1u );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( poseBefore, rig.m_Animator.GetAuthoringPose() ) )
         << "the first frame of the drag was captured as part of the 'before' and is now stuck";
}

// ── ONE TRANSACTION, WHATEVER IT CONTAINS ───────────────────────────────────────────────────────────

TEST_F( ClipEditUndo, ThreeChannelsAndAKeyAreStillOneStep )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::All );

    const LocalPose              poseBefore   = rig.m_Animator.GetAuthoringPose();
    const std::vector<BoneTrack> tracksBefore = rig.m_Clip.Tracks;

    // Translation, rotation AND scale in one drag, so the entry covers three channels plus the key.
    uint32_t pushed = rig.Frame( false, std::nullopt );
    for ( int i = 1; i <= 12; ++i )
    {
        const float t = static_cast<float>( i );
        glm::mat4   m = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 1.0f + 0.1f * t, 0.0f ) );
        m             = glm::rotate( m, 0.05f * t, glm::vec3( 0.0f, 0.0f, 1.0f ) );
        m             = glm::scale( m, glm::vec3( 1.0f + 0.01f * t ) );
        pushed += rig.Frame( true, m );
    }
    pushed += rig.Frame( false, std::nullopt );

    EXPECT_EQ( pushed, 1u );
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1u );

    const BoneTrack& child = rig.m_Clip.Tracks[0];
    ASSERT_EQ( child.PositionKeys.size(), 3u );
    ASSERT_EQ( child.RotationKeys.size(), 1u );
    ASSERT_EQ( child.ScaleKeys.size(), 1u );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( poseBefore, rig.m_Animator.GetAuthoringPose() ) );
    EXPECT_TRUE( SameTracks( tracksBefore, rig.m_Clip.Tracks ) );
}

TEST_F( ClipEditUndo, ADragThatMovedNothingIsNotAnUndoStep )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::All );

    // Held for ten frames without ever writing a pose: a click on the gizmo that missed.
    uint32_t pushed = rig.Frame( false, std::nullopt );
    for ( int i = 0; i < 10; ++i )
    {
        pushed += rig.Frame( true, std::nullopt );
    }
    pushed += rig.Frame( false, std::nullopt );

    EXPECT_EQ( pushed, 0u );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 0u );
}

// ── THE TWO DRIVERS DO NOT CLOSE EACH OTHER'S TRANSACTIONS ──────────────────────────────────────────

TEST_F( ClipEditUndo, ObserveDoesNotCommitATransactionBeginOpened )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::None );

    ASSERT_TRUE( rig.m_Transaction.Begin( &rig.m_Animator, &rig.m_Clip ).IsSuccess() );
    // A frame goes by with the manipulator idle. Before the driver was recorded, this read as a falling
    // edge and committed half of somebody else's edit.
    const auto observed = rig.m_Transaction.Observe( &rig.m_Animator, &rig.m_Clip, false );
    ASSERT_TRUE( observed.IsSuccess() );
    EXPECT_EQ( observed.GetValue(), 0u );
    EXPECT_TRUE( rig.m_Transaction.Open() );

    rig.m_Animator.SetBoneLocalPose( kChild, glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 9.0f, 0.0f ) ) );
    const auto ended = rig.m_Transaction.End();
    ASSERT_TRUE( ended.IsSuccess() );
    EXPECT_EQ( ended.GetValue(), 1u );
}

TEST_F( ClipEditUndo, ScopedEditMakesAButtonPressOneUndoStep )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::None );
    const std::vector<BoneTrack> tracksBefore = rig.m_Clip.Tracks;
    const LocalPose              poseBefore   = rig.m_Animator.GetAuthoringPose();

    {
        // "Key Bone @ Playhead": pose the bone, write the key, reload the pose from the clip.
        ScopedPoseEdit guard( rig.m_Transaction, &rig.m_Animator, &rig.m_Clip );
        rig.m_Animator.SetBoneLocalPose( kChild,
                                         glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 4.0f, 0.0f ) ) );
        rig.m_Animator.ApplyLocalPose();
        const auto keyed = rig.m_Keyer.WriteBone( rig.Target(), kChild );
        ASSERT_TRUE( keyed.IsSuccess() ) << keyed.GetError();
        ASSERT_EQ( keyed.GetValue(), 1u );
    }

    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1u );
    ASSERT_FALSE( SameTracks( tracksBefore, rig.m_Clip.Tracks ) );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameTracks( tracksBefore, rig.m_Clip.Tracks ) );
    EXPECT_TRUE( SameStoredValue( poseBefore, rig.m_Animator.GetAuthoringPose() ) );
}

TEST_F( ClipEditUndo, TransactionsDoNotNest )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::None );
    ASSERT_TRUE( rig.m_Transaction.Begin( &rig.m_Animator, &rig.m_Clip ).IsSuccess() );
    const auto second = rig.m_Transaction.Begin( &rig.m_Animator, &rig.m_Clip );
    EXPECT_FALSE( second.IsSuccess() );
    EXPECT_NE( second.GetError().find( "do not nest" ), std::string::npos );
}

TEST_F( ClipEditUndo, ATransactionWithNoClipIsRefusedRatherThanHalfRecorded )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::None );
    const auto began = rig.m_Transaction.Begin( &rig.m_Animator, nullptr );
    EXPECT_FALSE( began.IsSuccess() );
    EXPECT_FALSE( rig.m_Transaction.Open() );
}

// ── THE COMPARISON THE WHOLE FILE RESTS ON ──────────────────────────────────────────────────────────

TEST_F( ClipEditUndo, StoredValueComparesTheRepresentationAndNotJustTheAnimation )
{
    Desert::Animation::PositionKeyFrame a;
    a.Tick     = FrameNumber{ 7 };
    a.Position = glm::vec3( 1.0f, 2.0f, 3.0f );

    Desert::Animation::PositionKeyFrame b = a;
    EXPECT_TRUE( SameStoredValue( a, b ) );

    // A RESERVED FIELD IS STILL A BYTE AN UNDO OWES THE ANIMATOR. Two keys that sample identically today
    // are not the same stored key, and a comparison that said they were would let an undo skip a track.
    b.LeaveWeight = glm::vec3( 0.25f );
    EXPECT_FALSE( SameStoredValue( a, b ) );

    b             = a;
    b.ArriveTangent = glm::vec3( 0.0f, 0.0f, 1e-6f );
    EXPECT_FALSE( SameStoredValue( a, b ) ) << "tangents are what a neighbouring key's insertion changes";
}

TEST_F( ClipEditUndo, AnEntryRecordedAgainstAnotherRigIsDiscardedRatherThanApplied )
{
    Rig rig( MakeClipWithEndpoints(), AutoChangeMode::None );

    // An entry that believes the rig had three bones. `CommandHistory::Undo` discards a command that
    // reports failure and keeps walking down, which is how a stale entry is supposed to die.
    ClipPoseCommand stale( &rig.m_Animator, &rig.m_Clip, { ClipPoseCommand::BoneDelta{ 0, {}, {} } }, {}, 3, 3,
                           rig.m_Clip.Tracks.size(), rig.m_Clip.Tracks.size() );
    EXPECT_FALSE( stale.Undo() );
    EXPECT_FALSE( stale.Redo() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
