// HOW MANY UNDO STEPS IS ONE INTERACTION ON THE *UI* TIMELINE, AND WHAT DOES THE STEP PUT BACK.
//
// The headline number is measured in both directions, by running the same gesture twice:
//
//   * WITHOUT the transaction -- the gesture alone, which is exactly what `SequencerPanel::DrawUITracks`
//     did before this change -- the undo stack stays at 0 entries. That is the control, and it is not
//     proved by an empty stack: every such test first asserts that the gesture REALLY CHANGED the clip,
//     so "0 steps" means "the edit happened and nothing could take it back", not "nothing happened".
//   * WITH the transaction, the same gesture leaves 1.
//
// The round trip is asserted BY VALUE: `SameStoredValue` compares every field of every key of every
// lane, order included. And every round-trip test is guarded against being vacuous the way A22's was --
// it asserts the edit moved the thing it then undoes.
//
// THE GESTURES BELOW ARE COPIES OF THE PANEL'S OWN CODE, line for line, because `SequencerPanel.cpp` is
// compiled by no suite (scripts/CI/UnreachedSources.sh) and a double that edited the clip some OTHER way
// would be a second opinion about what one interaction does. What is left in the panel is the part that
// is genuinely ImGui: which widget's edges opened and closed the transaction.

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/UIClipEdit.hpp>

#include <Engine/ECS/Components.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using Desert::ECS::UIAnimData;
using Desert::ECS::UIAnimKey;
using Desert::ECS::UIAnimTrack;
using Desert::ECS::UIEasing;
using Desert::ECS::UITweenProperty;
using Desert::Editor::CaptureUIClip;
using Desert::Editor::CommandHistory;
using Desert::Editor::SameStoredValue;
using Desert::Editor::ScopedUIClipEdit;
using Desert::Editor::UIClipCommand;
using Desert::Editor::UIClipContent;
using Desert::Editor::UIClipEditTransaction;

namespace
{
    /// A clip with three lanes, each already holding two keys at both ends. The endpoints matter for the
    /// same reason they do in `ClipEditUndo`: an edit between them is an edit whose neighbours exist, so
    /// a restore that put back only the key that moved would be visibly short.
    UIAnimData MakeClip()
    {
        UIAnimData clip;
        clip.Duration = 2.0F;
        clip.Loop     = false;
        clip.Playing  = true;
        clip.Time     = 0.5F;

        const UITweenProperty properties[] = { UITweenProperty::Offset, UITweenProperty::Size,
                                               UITweenProperty::Opacity };
        for ( const UITweenProperty property : properties )
        {
            UIAnimTrack track;
            track.Property = property;
            track.Keys.push_back( { 0.0F, glm::vec4( 0.0F ), UIEasing::Linear } );
            track.Keys.push_back( { 2.0F, glm::vec4( 10.0F, 20.0F, 30.0F, 40.0F ), UIEasing::CubicOut } );
            clip.Tracks.push_back( std::move( track ) );
        }
        return clip;
    }

    /// `SequencerPanel::DrawUITracks`, the lane's "+" button: append at the playhead, carrying the last
    /// key's value, then re-sort the lane.
    void AddKeyAtPlayhead( UIAnimData& clip, size_t lane )
    {
        UIAnimTrack& track = clip.Tracks[lane];
        track.Keys.push_back( { clip.Time, track.Keys.empty() ? glm::vec4( 0.0F ) : track.Keys.back().Value,
                                UIEasing::CubicOut } );
        std::sort( track.Keys.begin(), track.Keys.end(),
                   []( const UIAnimKey& a, const UIAnimKey& b ) { return a.Time < b.Time; } );
    }

    /// The same panel's key drag, as it lands on the release edge: the key's time is written, then the
    /// lane is re-sorted because the key may have crossed a neighbour.
    void DragKeyTo( UIAnimData& clip, size_t lane, size_t key, float time )
    {
        UIAnimTrack& track   = clip.Tracks[lane];
        track.Keys[key].Time = time;
        clip.Playing         = false;    // the panel stops playback while a key is dragged
        clip.Time            = time;     // ...and the pose follows the key being moved
        std::sort( track.Keys.begin(), track.Keys.end(),
                   []( const UIAnimKey& a, const UIAnimKey& b ) { return a.Time < b.Time; } );
    }

    /// The "+ Track" button.
    void AddLane( UIAnimData& clip, UITweenProperty property )
    {
        UIAnimTrack track;
        track.Property = property;
        track.Keys.push_back( { 0.0F, glm::vec4( 0.0F ), UIEasing::CubicOut } );
        clip.Tracks.push_back( std::move( track ) );
    }

    size_t UndoDepth()
    {
        return CommandHistory::Get().UndoStack().size();
    }

    class UIClipUndoTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            CommandHistory::Get().Clear();
        }
        void TearDown() override
        {
            CommandHistory::Get().Clear();
        }
    };
} // namespace

// ── THE HEADLINE NUMBER ──────────────────────────────────────────────────────────────────────────────

TEST_F( UIClipUndoTest, TheSameGestureIsZeroUndoStepsWithoutTheTransactionAndOneWithIt )
{
    // WITHOUT. This is what the panel did before this change.
    {
        UIAnimData          clip   = MakeClip();
        const UIClipContent before = CaptureUIClip( clip );

        AddKeyAtPlayhead( clip, 0 );

        // The gesture REALLY HAPPENED -- without this the zero below would be the zero of a test that
        // edited nothing, which is the vacuous shape §8.4 is about.
        EXPECT_EQ( clip.Tracks[0].Keys.size(), 3U );
        EXPECT_FALSE( SameStoredValue( before, CaptureUIClip( clip ) ) );

        EXPECT_EQ( UndoDepth(), 0U ) << "the edit landed and nothing in the editor could take it back";
        EXPECT_FALSE( CommandHistory::Get().Undo() );
    }

    CommandHistory::Get().Clear();

    // WITH.
    {
        UIAnimData clip = MakeClip();

        UIClipEditTransaction transaction;
        ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
        AddKeyAtPlayhead( clip, 0 );
        const auto ended = transaction.End();
        ASSERT_TRUE( ended.IsSuccess() );
        EXPECT_EQ( ended.GetValue(), 1U );

        EXPECT_EQ( clip.Tracks[0].Keys.size(), 3U );
        EXPECT_EQ( UndoDepth(), 1U );
    }
}

TEST_F( UIClipUndoTest, ThreeLanesChangedInsideOneInteractionAreStillOneEntry )
{
    UIAnimData clip = MakeClip();

    UIClipEditTransaction transaction;
    ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
    AddKeyAtPlayhead( clip, 0 );
    AddKeyAtPlayhead( clip, 1 );
    AddKeyAtPlayhead( clip, 2 );
    ASSERT_TRUE( transaction.End().IsSuccess() );

    ASSERT_EQ( UndoDepth(), 1U );

    // AND THE ONE ENTRY IS CARRYING ALL THREE. An entry that pushed while holding nothing would satisfy
    // the count above and undo none of it.
    const auto* command = dynamic_cast<const UIClipCommand*>( CommandHistory::Get().UndoStack().back().get() );
    ASSERT_NE( command, nullptr );
    EXPECT_EQ( command->ChangedTracks(), 3U );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( clip.Tracks[0].Keys.size(), 2U );
    EXPECT_EQ( clip.Tracks[1].Keys.size(), 2U );
    EXPECT_EQ( clip.Tracks[2].Keys.size(), 2U );
}

// ── THE ROUND TRIP, BY VALUE ─────────────────────────────────────────────────────────────────────────

TEST_F( UIClipUndoTest, ADragRoundTripsThroughUndoAndRedoByValue )
{
    UIAnimData          clip   = MakeClip();
    const UIClipContent before = CaptureUIClip( clip );

    UIClipEditTransaction transaction;
    ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
    // Dragged PAST its neighbour, so the lane's order is part of what has to come back.
    DragKeyTo( clip, 0, 0, 1.75F );
    ASSERT_TRUE( transaction.End().IsSuccess() );

    const UIClipContent after = CaptureUIClip( clip );
    ASSERT_FALSE( SameStoredValue( before, after ) ) << "the drag must have moved something to undo";

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( SameStoredValue( before, CaptureUIClip( clip ) ) );

    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_TRUE( SameStoredValue( after, CaptureUIClip( clip ) ) );
}

TEST_F( UIClipUndoTest, ALaneCreatedByTheInteractionIsTakenAwayAgain )
{
    UIAnimData          clip   = MakeClip();
    const UIClipContent before = CaptureUIClip( clip );

    {
        UIClipEditTransaction transaction;
        const ScopedUIClipEdit step( transaction, &clip );
        AddLane( clip, UITweenProperty::Color );
    }

    ASSERT_EQ( clip.Tracks.size(), 4U );
    ASSERT_EQ( UndoDepth(), 1U );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( clip.Tracks.size(), 3U );
    EXPECT_TRUE( SameStoredValue( before, CaptureUIClip( clip ) ) );
}

TEST_F( UIClipUndoTest, DurationAndLoopRoundTrip )
{
    UIAnimData          clip   = MakeClip();
    const UIClipContent before = CaptureUIClip( clip );

    UIClipEditTransaction transaction;
    ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
    clip.Duration = 7.5F;
    clip.Loop     = true;
    ASSERT_TRUE( transaction.End().IsSuccess() );

    ASSERT_EQ( UndoDepth(), 1U );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_FLOAT_EQ( clip.Duration, 2.0F );
    EXPECT_FALSE( clip.Loop );
    EXPECT_TRUE( SameStoredValue( before, CaptureUIClip( clip ) ) );
}

// ── WHY IT IS A SNAPSHOT AND NOT AN INVERSE ──────────────────────────────────────────────────────────

TEST_F( UIClipUndoTest, KeysSharingATimeComeBackInTheirOwnOrder )
{
    // THE ARGUMENT AGAINST AN INVERSE COMMAND, AS A TEST. The lane below holds two keys at EXACTLY the
    // same time carrying different values -- an ordinary authored state, because the "+" button appends
    // at the playhead with no dedupe. An inverse spelled "remove the key at time T" names both of them
    // and has no way to choose; an inverse spelled "remove the key at index i" is addressing a position
    // the re-sort is free to move. The snapshot does not have to choose, and this is the assertion that
    // says so: the lane comes back key for key, value for value, IN ORDER.
    UIAnimData clip = MakeClip();
    clip.Time       = 1.0F;

    UIAnimTrack& lane = clip.Tracks[0];
    lane.Keys.clear();
    lane.Keys.push_back( { 1.0F, glm::vec4( 1.0F, 0.0F, 0.0F, 0.0F ), UIEasing::Linear } );
    lane.Keys.push_back( { 1.0F, glm::vec4( 2.0F, 0.0F, 0.0F, 0.0F ), UIEasing::BackOut } );

    const UIClipContent before = CaptureUIClip( clip );

    UIClipEditTransaction transaction;
    ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
    AddKeyAtPlayhead( clip, 0 ); // a THIRD key at the same time
    ASSERT_TRUE( transaction.End().IsSuccess() );

    ASSERT_EQ( clip.Tracks[0].Keys.size(), 3U );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    ASSERT_EQ( clip.Tracks[0].Keys.size(), 2U );
    EXPECT_TRUE( SameStoredValue( before, CaptureUIClip( clip ) ) );
    // Spelled out as well as compared, so a failure says WHICH of the two came back in the wrong place.
    EXPECT_FLOAT_EQ( clip.Tracks[0].Keys[0].Value.x, 1.0F );
    EXPECT_FLOAT_EQ( clip.Tracks[0].Keys[1].Value.x, 2.0F );
    EXPECT_EQ( clip.Tracks[0].Keys[1].Easing, UIEasing::BackOut );
}

// ── THE BOUNDARY: CONTENT IS RESTORED, TRANSPORT IS NOT ──────────────────────────────────────────────

TEST_F( UIClipUndoTest, ThePlayheadAndThePlayFlagAreDeliberatelyNotRestored )
{
    // The other half of the round trip, as a NEGATIVE CONTROL. `Time` is runtime-only (the serializer has
    // no such field) and `Playing` is transport that three separate places in the panel set false as a
    // side effect of the mouse being down. An undo that put those back would be undoing something nobody
    // authored -- so this test pins the boundary, and a future change that moves it has to move this line.
    UIAnimData clip = MakeClip();
    ASSERT_TRUE( clip.Playing );
    ASSERT_FLOAT_EQ( clip.Time, 0.5F );

    UIClipEditTransaction transaction;
    ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
    DragKeyTo( clip, 0, 0, 1.75F ); // also pauses playback and moves the playhead
    ASSERT_TRUE( transaction.End().IsSuccess() );

    ASSERT_TRUE( CommandHistory::Get().Undo() );

    EXPECT_FALSE( clip.Playing ) << "playback is transport, not authored content";
    EXPECT_FLOAT_EQ( clip.Time, 1.75F ) << "the playhead is runtime-only and undo must not rewind it";
    // ...and the content DID come back, which is what makes the two lines above a boundary rather than
    // a description of an undo that does nothing.
    EXPECT_FLOAT_EQ( clip.Tracks[0].Keys[0].Time, 0.0F );
}

// ── REFUSALS AND EMPTY INTERACTIONS ──────────────────────────────────────────────────────────────────

TEST_F( UIClipUndoTest, AnInteractionThatChangedNothingPushesNoEntry )
{
    UIAnimData clip = MakeClip();

    UIClipEditTransaction transaction;
    ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
    clip.Time    = 1.9F; // scrubbing...
    clip.Playing = false;
    const auto ended = transaction.End();
    ASSERT_TRUE( ended.IsSuccess() );
    EXPECT_EQ( ended.GetValue(), 0U );
    EXPECT_EQ( UndoDepth(), 0U );
}

TEST_F( UIClipUndoTest, TransactionsDoNotNestAndAnEndWithoutABeginIsRefused )
{
    UIAnimData clip = MakeClip();

    UIClipEditTransaction transaction;
    EXPECT_FALSE( transaction.End().IsSuccess() );
    ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
    EXPECT_FALSE( transaction.Begin( &clip ).IsSuccess() );
    EXPECT_FALSE( transaction.Begin( nullptr ).IsSuccess() );
    transaction.Cancel();
    EXPECT_FALSE( transaction.Open() );
    EXPECT_EQ( UndoDepth(), 0U );
}

TEST_F( UIClipUndoTest, TheSubjectIsReadableSoAStaleTransactionCanBeAbandoned )
{
    UIAnimData first;
    UIAnimData second;

    UIClipEditTransaction transaction;
    ASSERT_TRUE( transaction.Begin( &first ).IsSuccess() );
    EXPECT_EQ( transaction.Subject(), &first );
    EXPECT_NE( transaction.Subject(), &second );
    transaction.Cancel();
    EXPECT_EQ( transaction.Subject(), nullptr );
}

TEST_F( UIClipUndoTest, TheEntryIsVolatileAndDropVolatileTakesIt )
{
    // The pointer guard, asserted rather than described: the command holds a UIAnimData* into a live
    // component, and entt relocates a pool when it grows, so the address can die while the ENTITY lives.
    UIAnimData clip = MakeClip();

    UIClipEditTransaction transaction;
    ASSERT_TRUE( transaction.Begin( &clip ).IsSuccess() );
    AddKeyAtPlayhead( clip, 0 );
    ASSERT_TRUE( transaction.End().IsSuccess() );

    ASSERT_EQ( UndoDepth(), 1U );
    EXPECT_TRUE( CommandHistory::Get().UndoStack().back()->IsVolatile() );

    CommandHistory::Get().DropVolatile();
    EXPECT_EQ( UndoDepth(), 0U );
}
