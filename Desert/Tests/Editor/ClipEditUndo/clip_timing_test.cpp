// ANIM-FIX9: the clip's Length and Display Rate are edits — one undo record each, no key moves in time (keys
// are ticks on TickRate), and the file's sequence carries both (WriteSequence/ReadSequence: what Save and a
// reload go through for the `.anim`'s sequence).

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Panels/AnimationEditor/ClipTiming.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <gtest/gtest.h>

#include <span>

using Desert::Animation::AnimationClip;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameRate;
using Desert::Animation::FrameTime;
using Desert::Animation::FrameTimeToSeconds;
using Desert::Editor::ClipCurves;
using Desert::Editor::ClipLengthTicks;
using Desert::Editor::CommandHistory;
using Desert::Editor::CurveKeys;

namespace
{
    // A one-second clip at 30 fps with a curve key at 0.5 s (12000 ticks) and one at 1.0 s.
    AnimationClip OneSecondClip()
    {
        AnimationClip clip;
        clip.Sequence.TickRate    = FrameRate{ 24000, 1 };
        clip.Sequence.DisplayRate = FrameRate{ 30, 1 };
        clip.Sequence.End         = FrameNumber{ 24000 };
        (void)Desert::Editor::SetCurveKey( clip, "Weight", FrameNumber{ 12000 }, 0.5f,
                                           Desert::Animation::KeyInterp::Cubic, {} );
        (void)Desert::Editor::SetCurveKey( clip, "Weight", FrameNumber{ 24000 }, 1.0f,
                                           Desert::Animation::KeyInterp::Cubic, {} );
        return clip;
    }

    std::vector<double> KeySeconds( const AnimationClip& clip )
    {
        std::vector<double> seconds;
        for ( const auto& key : CurveKeys( *ClipCurves( clip ).front() ) )
            seconds.push_back( FrameTimeToSeconds( FrameTime{ key.Tick, 0.0F }, clip.Sequence.TickRate ) );
        return seconds;
    }

    struct ClipTiming : ::testing::Test
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

TEST_F( ClipTiming, LengthAndRateAreOneUndoRecordEachAndNoKeyMovesInTime )
{
    AnimationClip clip = OneSecondClip();
    CommandHistory::Get().Clear();
    const auto keysBefore = KeySeconds( clip );
    ASSERT_EQ( keysBefore.size(), 2u );

    // 1.5 s typed is the nearest tick, 36000 — not the 35999 a floor through a double can give.
    ASSERT_EQ( ClipLengthTicks( clip, 1.5 ).Value, 36000 );
    ASSERT_TRUE( Desert::Editor::SetClipLength( clip, ClipLengthTicks( clip, 1.5 ), {} ) );
    EXPECT_EQ( clip.Sequence.Start.Value, 0 );
    EXPECT_EQ( clip.Sequence.End.Value, 36000 );
    EXPECT_DOUBLE_EQ( clip.DurationSeconds(), 1.5 );
    EXPECT_EQ( KeySeconds( clip ), keysBefore );

    ASSERT_TRUE( Desert::Editor::SetClipDisplayRate( clip, FrameRate{ 60, 1 }, {} ) );
    EXPECT_EQ( clip.Sequence.DisplayRate, ( FrameRate{ 60, 1 } ) );
    EXPECT_EQ( clip.Sequence.TickRate, ( FrameRate{ 24000, 1 } ) );
    EXPECT_EQ( KeySeconds( clip ), keysBefore );
    EXPECT_DOUBLE_EQ( clip.DurationSeconds(), 1.5 );

    // No change, no record; nonsense refused without a record.
    EXPECT_FALSE( Desert::Editor::SetClipLength( clip, FrameNumber{ 36000 }, {} ) );
    EXPECT_FALSE( Desert::Editor::SetClipLength( clip, FrameNumber{ 0 }, {} ) );
    EXPECT_FALSE( Desert::Editor::SetClipDisplayRate( clip, FrameRate{ 120, 2 }, {} ) ); // = 60/1
    EXPECT_FALSE( Desert::Editor::SetClipDisplayRate( clip, FrameRate{ 0, 1 }, {} ) );

    ASSERT_TRUE( CommandHistory::Get().Undo() ); // the rate
    EXPECT_EQ( clip.Sequence.DisplayRate, ( FrameRate{ 30, 1 } ) );
    EXPECT_EQ( clip.Sequence.End.Value, 36000 );
    ASSERT_TRUE( CommandHistory::Get().Undo() ); // the length
    EXPECT_EQ( clip.Sequence.End.Value, 24000 );
    EXPECT_EQ( KeySeconds( clip ), keysBefore );
    EXPECT_FALSE( CommandHistory::Get().Undo() );

    ASSERT_TRUE( CommandHistory::Get().Redo() );
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_EQ( clip.Sequence.End.Value, 36000 );
    EXPECT_EQ( clip.Sequence.DisplayRate, ( FrameRate{ 60, 1 } ) );
}

TEST_F( ClipTiming, TheSavedSequenceReadsBackWithTheNewLengthAndRate )
{
    AnimationClip clip = OneSecondClip();
    ASSERT_TRUE( Desert::Editor::SetClipLength( clip, ClipLengthTicks( clip, 1.5 ), {} ) );
    ASSERT_TRUE( Desert::Editor::SetClipDisplayRate( clip, FrameRate{ 60, 1 }, {} ) );

    const auto written = Desert::Animation::Timeline::WriteSequence( clip.Sequence );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    const auto& bytes = written.GetValue();
    auto        read  = Desert::Animation::Timeline::ReadSequence( std::span<const uint8_t>( bytes ) );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();

    AnimationClip reloaded;
    reloaded.Sequence = read.ExtractValue();
    EXPECT_EQ( reloaded.Sequence.End.Value, 36000 );
    EXPECT_DOUBLE_EQ( reloaded.DurationSeconds(), 1.5 );
    EXPECT_EQ( reloaded.Sequence.DisplayRate, ( FrameRate{ 60, 1 } ) );
    EXPECT_EQ( KeySeconds( reloaded ), KeySeconds( clip ) );
    EXPECT_FALSE( Desert::Editor::ClipDiffersFromFile( clip, reloaded ) );
}
