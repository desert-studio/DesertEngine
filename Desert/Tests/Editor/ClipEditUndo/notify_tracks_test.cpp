// ANV1b: the Animation Editor's Notifies tracks — every edit is ONE undo record holding the list before and
// after, a dragged notify lands on the display grid inside the clip, and the marker lit while the clip plays
// is the one the Animator fires (both read Animation::NotifyCrossed).

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Panels/AnimationEditor/AnimationNotifyTracks.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <gtest/gtest.h>

#include <vector>

using Desert::Animation::AnimationClip;
using Desert::Animation::AnimationNotify;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameRate;
using Desert::Editor::CommandHistory;

namespace
{
    AnimationClip ThreeNotifies()
    {
        AnimationClip clip;
        clip.TickRate      = FrameRate{ 24000, 1 };
        clip.DisplayRate   = FrameRate{ 30, 1 };
        clip.DurationTicks = FrameNumber{ 48000 }; // 2 s
        clip.Notifies      = { { "L", FrameNumber{ 4000 }, 0 },
                               { "R", FrameNumber{ 24000 }, 0 },
                               { "Sync", FrameNumber{ 40000 }, 1 } };
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
    const auto    before  = clip.Notifies;
    int           changes = 0;

    auto edited        = clip.Notifies;
    edited[0].Tick     = FrameNumber{ 32000 }; // "L" moves past "R"
    edited[0].Track    = 2;
    const bool applied = Desert::Editor::ApplyNotifyEdit( clip, edited, "Move Notify", CommandHistory::Get(),
                                                          [&changes]() { ++changes; } );
    ASSERT_TRUE( applied );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u );
    EXPECT_EQ( changes, 1 );
    // Kept in tick order: the Animator scans the list in order.
    ASSERT_EQ( clip.Notifies.size(), 3u );
    EXPECT_EQ( clip.Notifies[0].Name, "R" );
    EXPECT_EQ( clip.Notifies[1].Name, "L" );
    EXPECT_EQ( clip.Notifies[1].Track, 2 );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( changes, 2 );
    ASSERT_EQ( clip.Notifies.size(), before.size() );
    for ( size_t i = 0; i < before.size(); ++i )
    {
        EXPECT_EQ( clip.Notifies[i].Name, before[i].Name );
        EXPECT_EQ( clip.Notifies[i].Tick.Value, before[i].Tick.Value );
        EXPECT_EQ( clip.Notifies[i].Track, before[i].Track );
    }
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_EQ( clip.Notifies[1].Name, "L" );
}

TEST_F( NotifyTracks, AddRenameDeleteAreOneRecordEachAndANoOpIsNone )
{
    AnimationClip clip  = ThreeNotifies();
    auto          added = clip.Notifies;
    added.push_back( { "Hit", FrameNumber{ 800 }, 1 } );
    ASSERT_TRUE( Desert::Editor::ApplyNotifyEdit( clip, added, "Add Notify", CommandHistory::Get(), {} ) );
    EXPECT_EQ( clip.Notifies.front().Name, "Hit" );

    auto renamed    = clip.Notifies;
    renamed[0].Name = "Impact";
    ASSERT_TRUE( Desert::Editor::ApplyNotifyEdit( clip, renamed, "Rename Notify", CommandHistory::Get(), {} ) );

    auto removed = clip.Notifies;
    removed.erase( removed.begin() );
    ASSERT_TRUE( Desert::Editor::ApplyNotifyEdit( clip, removed, "Delete Notify", CommandHistory::Get(), {} ) );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 3u );

    EXPECT_FALSE( Desert::Editor::ApplyNotifyEdit( clip, clip.Notifies, "Nothing", CommandHistory::Get(), {} ) );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 3u );

    ASSERT_TRUE( CommandHistory::Get().Undo() ); // the delete
    EXPECT_EQ( clip.Notifies.front().Name, "Impact" );
    ASSERT_TRUE( CommandHistory::Get().Undo() ); // the rename
    EXPECT_EQ( clip.Notifies.front().Name, "Hit" );
}

TEST_F( NotifyTracks, ADraggedNotifyLandsOnTheDisplayGridInsideTheClip )
{
    const AnimationClip clip = ThreeNotifies();
    // 0.51 s at 30 fps is between frames 15 (0.5 s) and 16; it lands on frame 15 = tick 12000.
    EXPECT_EQ( Desert::Editor::SnapNotifyTick( 0.51, clip.TickRate, clip.DisplayRate, clip.DurationTicks ).Value,
               12000 );
    EXPECT_EQ( Desert::Editor::SnapNotifyTick( -0.3, clip.TickRate, clip.DisplayRate, clip.DurationTicks ).Value,
               0 );
    EXPECT_EQ( Desert::Editor::SnapNotifyTick( 9.0, clip.TickRate, clip.DisplayRate, clip.DurationTicks ).Value,
               48000 );
}

TEST_F( NotifyTracks, RowsAreEveryUsedTrackTheAddedOnesAndAtLeastOne )
{
    const AnimationClip clip = ThreeNotifies();
    EXPECT_EQ( Desert::Editor::NotifyTrackCount( clip.Notifies, 0 ), 2 );
    EXPECT_EQ( Desert::Editor::NotifyTrackCount( clip.Notifies, 4 ), 4 );
    EXPECT_EQ( Desert::Editor::NotifyTrackCount( {}, 0 ), 1 );
}

TEST_F( NotifyTracks, TheLitNotifyIsTheOneThePlayheadCrossed )
{
    const AnimationClip clip = ThreeNotifies();
    // 0.9 s -> 1.1 s crosses "R" at 1.0 s only.
    auto lit = Desert::Editor::CrossedNotifies( clip.Notifies, clip.TickRate, 0.9, 1.1, false );
    ASSERT_EQ( lit.size(), 1u );
    EXPECT_EQ( clip.Notifies[lit[0]].Name, "R" );
    // A scrub to the left crosses nothing, like playback, which never fires backwards.
    EXPECT_TRUE( Desert::Editor::CrossedNotifies( clip.Notifies, clip.TickRate, 1.1, 0.9, false ).empty() );
    // A loop wrap 1.9 s -> 0.2 s crosses "L" (0.167 s) and not "Sync" (1.667 s).
    lit = Desert::Editor::CrossedNotifies( clip.Notifies, clip.TickRate, 1.9, 0.2, true );
    ASSERT_EQ( lit.size(), 1u );
    EXPECT_EQ( clip.Notifies[lit[0]].Name, "L" );
}
