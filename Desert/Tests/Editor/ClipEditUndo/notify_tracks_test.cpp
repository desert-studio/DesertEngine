// ANV1b: the Animation Editor's Notifies tracks — every edit is ONE undo record holding the list before and
// after, a dragged notify lands on the display grid inside the clip, and the marker lit while the clip plays
// is the one the Animator fires (both read Timeline::CollectCrossed). The notifies are the Event keys of the
// clip's Sequence (ANIM-I10d); every edit is the SequenceEditCommand every clip edit records.

#include <Engine/Core/WindowCloseGate.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/UnsavedClose.hpp>
#include <Editor/Panels/AnimationEditor/AnimationNotifyTracks.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using Desert::Animation::AnimationClip;
using NotifyKey = Desert::Animation::Timeline::EventKey;
using Desert::Editor::ClipCurves;
using Desert::Editor::ClipNotifies;
using Desert::Editor::CurveKeys;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameRate;
using Desert::Editor::CommandHistory;

namespace
{
    AnimationClip ThreeNotifies()
    {
        AnimationClip clip;
        clip.Sequence.TickRate    = FrameRate{ 24000, 1 };
        clip.Sequence.DisplayRate = FrameRate{ 30, 1 };
        clip.Sequence.End         = FrameNumber{ 48000 }; // 2 s
        Desert::Editor::NotifyTrackDetail::FirstChannelForEdit<Desert::Animation::Timeline::EventChannel>(
             clip.Sequence, Desert::Animation::Timeline::TrackKind::Event, "" )
             .Keys = { NotifyKey{ FrameNumber{ 4000 }, FrameNumber{ 0 }, "L", 0 },
                       NotifyKey{ FrameNumber{ 24000 }, FrameNumber{ 0 }, "R", 0 },
                       NotifyKey{ FrameNumber{ 40000 }, FrameNumber{ 0 }, "Sync", 1 } };
        return clip;
    }

    struct NotifyTracks : ::testing::Test
    {
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

TEST_F( NotifyTracks, AMoveIsOneUndoRecordAndUndoPutsTheListBack )
{
    AnimationClip clip    = ThreeNotifies();
    const auto    before  = ClipNotifies( clip );
    int           changes = 0;

    auto edited        = ClipNotifies( clip );
    edited[0].Tick     = FrameNumber{ 32000 }; // "L" moves past "R"
    edited[0].Row    = 2;
    const bool applied = Desert::Editor::ApplyNotifyEdit( clip, edited, [&changes]() { ++changes; } );
    ASSERT_TRUE( applied );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u );
    EXPECT_EQ( changes, 1 );
    // Kept in tick order: the Animator scans the list in order.
    ASSERT_EQ( ClipNotifies( clip ).size(), 3u );
    EXPECT_EQ( ClipNotifies( clip )[0].Name, "R" );
    EXPECT_EQ( ClipNotifies( clip )[1].Name, "L" );
    EXPECT_EQ( ClipNotifies( clip )[1].Row, 2 );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( changes, 2 );
    ASSERT_EQ( ClipNotifies( clip ).size(), before.size() );
    for ( size_t i = 0; i < before.size(); ++i )
    {
        EXPECT_EQ( ClipNotifies( clip )[i].Name, before[i].Name );
        EXPECT_EQ( ClipNotifies( clip )[i].Tick.Value, before[i].Tick.Value );
        EXPECT_EQ( ClipNotifies( clip )[i].Row, before[i].Row );
    }
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_EQ( ClipNotifies( clip )[1].Name, "L" );
}

TEST_F( NotifyTracks, AddRenameDeleteAreOneRecordEachAndANoOpIsNone )
{
    AnimationClip clip  = ThreeNotifies();
    auto          added = ClipNotifies( clip );
    added.push_back( NotifyKey{ FrameNumber{ 800 }, FrameNumber{ 0 }, "Hit", 1 } );
    ASSERT_TRUE( Desert::Editor::ApplyNotifyEdit( clip, added, {} ) );
    EXPECT_EQ( ClipNotifies( clip ).front().Name, "Hit" );

    auto renamed    = ClipNotifies( clip );
    renamed[0].Name = "Impact";
    ASSERT_TRUE( Desert::Editor::ApplyNotifyEdit( clip, renamed, {} ) );

    auto removed = ClipNotifies( clip );
    removed.erase( removed.begin() );
    ASSERT_TRUE( Desert::Editor::ApplyNotifyEdit( clip, removed, {} ) );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 3u );

    EXPECT_FALSE( Desert::Editor::ApplyNotifyEdit( clip, ClipNotifies( clip ), {} ) );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 3u );

    ASSERT_TRUE( CommandHistory::Get().Undo() ); // the delete
    EXPECT_EQ( ClipNotifies( clip ).front().Name, "Impact" );
    ASSERT_TRUE( CommandHistory::Get().Undo() ); // the rename
    EXPECT_EQ( ClipNotifies( clip ).front().Name, "Hit" );
}

TEST_F( NotifyTracks, ADraggedNotifyLandsOnTheDisplayGridInsideTheClip )
{
    const AnimationClip clip = ThreeNotifies();
    // 0.51 s at 30 fps is between frames 15 (0.5 s) and 16; it lands on frame 15 = tick 12000.
    EXPECT_EQ( Desert::Editor::SnapNotifyTick( 0.51, clip.Sequence.TickRate, clip.Sequence.DisplayRate, clip.DurationTicks() ).Value,
               12000 );
    EXPECT_EQ( Desert::Editor::SnapNotifyTick( -0.3, clip.Sequence.TickRate, clip.Sequence.DisplayRate, clip.DurationTicks() ).Value,
               0 );
    EXPECT_EQ( Desert::Editor::SnapNotifyTick( 9.0, clip.Sequence.TickRate, clip.Sequence.DisplayRate, clip.DurationTicks() ).Value,
               48000 );
}

TEST_F( NotifyTracks, RowsAreEveryUsedTrackTheAddedOnesAndAtLeastOne )
{
    const AnimationClip clip = ThreeNotifies();
    EXPECT_EQ( Desert::Editor::NotifyTrackCount( ClipNotifies( clip ), 0 ), 2 );
    EXPECT_EQ( Desert::Editor::NotifyTrackCount( ClipNotifies( clip ), 4 ), 4 );
    EXPECT_EQ( Desert::Editor::NotifyTrackCount( {}, 0 ), 1 );
}

TEST_F( NotifyTracks, TheLitNotifyIsTheOneThePlayheadCrossed )
{
    const AnimationClip clip = ThreeNotifies();
    // 0.9 s -> 1.1 s crosses "R" at 1.0 s only.
    auto lit = Desert::Editor::CrossedNotifies( clip, 0.9, 1.1, false );
    ASSERT_EQ( lit.size(), 1u );
    EXPECT_EQ( ClipNotifies( clip )[lit[0]].Name, "R" );
    // A scrub to the left crosses nothing, like playback, which never fires backwards.
    EXPECT_TRUE( Desert::Editor::CrossedNotifies( clip, 1.1, 0.9, false ).empty() );
    // A loop wrap 1.9 s -> 0.2 s crosses "L" (0.167 s) and not "Sync" (1.667 s).
    lit = Desert::Editor::CrossedNotifies( clip, 1.9, 0.2, true );
    ASSERT_EQ( lit.size(), 1u );
    EXPECT_EQ( ClipNotifies( clip )[lit[0]].Name, "L" );
}

// ANV3: a notify's length (UE's Notify State) and an anim curve's key are one undo record each.
TEST_F( NotifyTracks, ANotifyLengthIsOneUndoRecord )
{
    AnimationClip clip = ThreeNotifies();
    ASSERT_TRUE( Desert::Editor::SetNotifyDuration( clip, 0, FrameNumber{ 4800 }, {} ) );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u );
    EXPECT_EQ( ClipNotifies( clip )[0].Duration.Value, 4800 );
    EXPECT_FALSE( Desert::Editor::SetNotifyDuration( clip, 0, FrameNumber{ 4800 }, {} ) )
         << "the same length again is no edit";
    EXPECT_FALSE( Desert::Editor::SetNotifyDuration( clip, 0, FrameNumber{ -1 }, {} ) );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( ClipNotifies( clip )[0].Duration.Value, 0 );
}

TEST_F( NotifyTracks, ACurveKeyIsOneUndoRecordAndCreatesTheCurve )
{
    AnimationClip clip = ThreeNotifies();
    using Desert::Animation::KeyInterp;
    ASSERT_TRUE( Desert::Editor::SetCurveKey( clip, "Blink", FrameNumber{ 2400 }, 1.0F, KeyInterp::Cubic, {} ) );
    ASSERT_TRUE( Desert::Editor::SetCurveKey( clip, "Blink", FrameNumber{ 0 }, 0.5F, KeyInterp::Linear, {} ) );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 2u );
    ASSERT_EQ( ClipCurves( clip ).size(), 1u );
    ASSERT_EQ( CurveKeys( *ClipCurves( clip )[0] ).size(), 2u );
    EXPECT_EQ( CurveKeys( *ClipCurves( clip )[0] )[0].Tick.Value, 0 ) << "keys are kept in tick order";

    ASSERT_TRUE( Desert::Editor::RemoveCurveKey( clip, "Blink", FrameNumber{ 2400 }, {} ) );
    EXPECT_EQ( CurveKeys( *ClipCurves( clip )[0] ).size(), 1u );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( CurveKeys( *ClipCurves( clip )[0] ).size(), 2u );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_TRUE( ClipCurves( clip ).empty() );
}

// The rule behind "Save*": the clip in memory against the clip its FILE holds. A window reopened on an edited
// clip compares with the file, so the edit is still dirty (ANV1c3 lost it by comparing with a reopen snapshot).
TEST_F( NotifyTracks, AnEditIsDirtyAgainstTheFileAndUndoMakesItCleanAgain )
{
    const AnimationClip onDisk = ThreeNotifies();
    AnimationClip       clip   = onDisk;
    EXPECT_FALSE( Desert::Editor::ClipDiffersFromFile( clip, onDisk ) );

    ASSERT_TRUE( Desert::Editor::SetNotifyDuration( clip, 0, FrameNumber{ 8000 }, {} ) );
    EXPECT_TRUE( Desert::Editor::ClipDiffersFromFile( clip, onDisk ) ) << "a state's LENGTH is authoring too";
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_FALSE( Desert::Editor::ClipDiffersFromFile( clip, onDisk ) );

    ASSERT_TRUE( Desert::Editor::SetCurveKey( clip, "Weight", FrameNumber{ 0 }, 0.5f,
                                              Desert::Animation::KeyInterp::Linear, {} ) );
    EXPECT_TRUE( Desert::Editor::ClipDiffersFromFile( clip, onDisk ) ) << "a curve key is authoring too";
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_FALSE( Desert::Editor::ClipDiffersFromFile( clip, onDisk ) );
}

TEST_F( NotifyTracks, DraggingAStateEdgeMovesOnlyThatEdgeAndKeepsOneTick )
{
    using Desert::Editor::DragNotifyStateEdge;
    using Desert::Editor::NotifyStateEdge;
    const NotifyKey state{ FrameNumber{ 8000 }, FrameNumber{ 8000 }, "FootPlant", 0 }; // [8000, 16000)
    const FrameNumber     duration{ 48000 };

    const auto endMoved = DragNotifyStateEdge( state, NotifyStateEdge::End, FrameNumber{ 20000 }, duration );
    EXPECT_EQ( endMoved.Tick.Value, 8000 );
    EXPECT_EQ( endMoved.Duration.Value, 12000 );

    const auto beginMoved = DragNotifyStateEdge( state, NotifyStateEdge::Begin, FrameNumber{ 4000 }, duration );
    EXPECT_EQ( beginMoved.Tick.Value, 4000 );
    EXPECT_EQ( beginMoved.Duration.Value, 12000 ) << "the end stays at 16000";

    const auto crushed = DragNotifyStateEdge( state, NotifyStateEdge::Begin, FrameNumber{ 30000 }, duration );
    EXPECT_EQ( crushed.Tick.Value, 15999 );
    EXPECT_EQ( crushed.Duration.Value, 1 ) << "a state never collapses into an instant by a drag";

    const auto past = DragNotifyStateEdge( state, NotifyStateEdge::End, FrameNumber{ 90000 }, duration );
    EXPECT_EQ( past.Tick.Value + past.Duration.Value, 48000 ) << "the span stays inside the clip";
}

// ANV1d2: A CLOSED ANIMATION EDITOR TAKES ITS UNDO RECORDS WITH IT. The history is process-wide; the clip's
// payload is not (the window's root pin goes, the manager may evict it). Closing drops every record that writes
// into that clip, so an Undo after the close walks past them and never reaches freed memory (ASan-clean).
TEST_F( NotifyTracks, AClosedEditorsRecordsLeaveTheHistoryAndUndoNeverReachesItsClip )
{
    auto          closing = std::make_unique<AnimationClip>( ThreeNotifies() );
    AnimationClip other   = ThreeNotifies();
    auto          edited  = ClipNotifies( *closing );
    edited[0].Tick        = FrameNumber{ 8000 };
    ASSERT_TRUE( Desert::Editor::ApplyNotifyEdit( other, edited, {} ) );
    ASSERT_TRUE( Desert::Editor::ApplyNotifyEdit( *closing, edited, {} ) );
    ASSERT_TRUE( Desert::Editor::SetCurveKey( *closing, "Blink", FrameNumber{ 2400 }, 1.0F,
                                              Desert::Animation::KeyInterp::Cubic, {} ) );
    ASSERT_TRUE( CommandHistory::Get().Undo() ); // the curve key, into the redo stack
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 2u );
    EXPECT_EQ( CommandHistory::Get().RedoStack().size(), 1u );

    CommandHistory::Get().DropFor( closing.get() );
    closing.reset(); // the payload dies with the window

    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u ) << "only the other clip's record is left";
    EXPECT_TRUE( CommandHistory::Get().RedoStack().empty() ) << "a redo into the dead clip is gone too";
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( ClipNotifies( other )[0].Tick.Value, 4000 ) << "the surviving record still undoes its own clip";
    EXPECT_FALSE( CommandHistory::Get().Undo() );
}

// ANV1d2: the close question's rule - asked only of a dirty document the person closed; Save closes only when
// the file was written, Don't Save always closes, Cancel never does.
TEST( UnsavedClose, AsksOnlyADirtyDocumentThePersonClosedAndClosesPerAnswer )
{
    using Desert::Editor::CloseAfterAnswer;
    using Desert::Editor::CloseAsksFirst;
    using Desert::Editor::UnsavedCloseChoice;
    EXPECT_TRUE( CloseAsksFirst( true, true ) );
    EXPECT_FALSE( CloseAsksFirst( false, true ) ) << "a clean or untracked document closes without a question";
    EXPECT_FALSE( CloseAsksFirst( true, false ) ) << "a close the editor makes itself cannot ask anyone";
    EXPECT_TRUE( CloseAfterAnswer( UnsavedCloseChoice::Save, true ) );
    EXPECT_FALSE( CloseAfterAnswer( UnsavedCloseChoice::Save, false ) ) << "a failed save keeps the window";
    EXPECT_TRUE( CloseAfterAnswer( UnsavedCloseChoice::Discard, false ) );
    EXPECT_FALSE( CloseAfterAnswer( UnsavedCloseChoice::Cancel, true ) );
}

// ANV4: the OS frame's close is the editor's to decide -- the same question File > Exit asks.
TEST( WindowCloseGate, WithoutAnOwnerTheFrameStopsTheRunAndWithOneTheOwnerDecides )
{
    Desert::Core::WindowCloseGate gate;
    EXPECT_TRUE( gate.StopsNow() );

    int asked = 0;
    gate.Install(
         [&asked]()
         {
             ++asked;
             return false;
         } ); // questions raised: Cancel must be possible
    EXPECT_FALSE( gate.StopsNow() );
    EXPECT_EQ( asked, 1 );

    gate.Install(
         [&asked]()
         {
             ++asked;
             return true;
         } );
    EXPECT_TRUE( gate.StopsNow() );
    EXPECT_EQ( asked, 2 );

    gate.Uninstall();
    EXPECT_TRUE( gate.StopsNow() );
    EXPECT_EQ( asked, 2 );
}
