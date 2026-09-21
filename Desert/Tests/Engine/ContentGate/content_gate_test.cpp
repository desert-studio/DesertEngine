// THE RULE THAT DECIDES WHETHER A PLAYER SEES A HALF-READ WORLD.
//
// `ContentGate` is the state both hosts hold while demand-driven content is still arriving: the editor
// keeps its loading overlay up for it, and the shipping runtime does not blit the scene into the
// swapchain at all until it opens. Everything about whether that works reduces to WHEN IT OPENS.
//
// The dangerous shape is the one-condition gate. "Nothing outstanding" alone looks like the whole
// answer and is not: a read that has just COMPLETED is the most likely reason the next one is about to
// be made -- a cloud type arrives, and only then is there a handle to ask the noise service with -- so
// a queue can be empty in the GAP between two links of a chain. A gate that opened there would hand
// over a world one link short, which is the original defect with a lower probability, i.e. worse.
//
// So these tests are mostly about the second condition, and the one that matters most is
// OpensNotInTheGapBetweenTwoLinks: delete `startedCount == m_StartedAtFrameBegin` from the
// implementation and it is the test that goes red.

#include <Engine/Assets/ContentGate.hpp>

#include <gtest/gtest.h>

#include <thread>

using Desert::Assets::ContentGate;
using Desert::Assets::ContentState;

TEST( ContentGate, AGateConstructedReadyIsNeverClosedByTraffic )
{
    // The editor's case: it opens on an empty scene behind its own staged-boot overlay, and nothing but
    // a scene load (BeginWorld) may put it into the wait. Reads started by a thumbnail or an import must
    // not black out the viewport.
    ContentGate gate( ContentState::Ready );
    EXPECT_FALSE( gate.Loading() );
    EXPECT_FALSE( gate.Tick( 9, 100 ) );
    EXPECT_FALSE( gate.Tick( 0, 100 ) );
    EXPECT_FALSE( gate.Loading() );
}

TEST( ContentGate, AGateConstructedLoadingStartsShut )
{
    // The runtime's case, and the reason the initial state is a constructor argument rather than a
    // default: this host exists in order to read a world, and its first frames are the ones the whole
    // type was added to cover.
    const ContentGate gate( ContentState::Loading );
    EXPECT_TRUE( gate.Loading() );
    EXPECT_EQ( gate.State(), ContentState::Loading );
    EXPECT_EQ( gate.FramesWaited(), 0u );
}

TEST( ContentGate, DoesNotOpenOnTheFirstFrameEvenWhenEverythingIsQuiet )
{
    // THE FIRST FRAME IS WHERE THE RENDERER ASKS. An empty queue before anything has looked at the scene
    // says only that nobody has looked yet -- the same mistake as reading a screenshot at three frames
    // on a swapchain with three frames in flight. This is also the floor: a world that wants nothing
    // pays exactly two frames of loading screen and not one more.
    ContentGate gate( ContentState::Loading );
    EXPECT_FALSE( gate.Tick( 0, 0 ) ) << "the gate opened before a single frame had asked for anything";
    EXPECT_TRUE( gate.Loading() );
    EXPECT_EQ( gate.FramesWaited(), 1u );

    EXPECT_TRUE( gate.Tick( 0, 0 ) );
    EXPECT_FALSE( gate.Loading() );
    EXPECT_EQ( gate.FramesWaited(), ContentGate::kQuietFramesToOpen );
}

TEST( ContentGate, AnOutstandingReadHoldsItShut )
{
    ContentGate gate( ContentState::Loading );
    EXPECT_FALSE( gate.Tick( 3, 3 ) );
    EXPECT_FALSE( gate.Tick( 2, 3 ) );
    EXPECT_FALSE( gate.Tick( 1, 3 ) );
    EXPECT_TRUE( gate.Loading() );
    // The queue is empty now, but the streak has only just started: the frame that follows this tick has
    // not been seen yet, and it is the frame in which whatever just landed gets USED.
    EXPECT_FALSE( gate.Tick( 0, 3 ) );
    EXPECT_TRUE( gate.Tick( 0, 3 ) );
    EXPECT_FALSE( gate.Loading() );
}

TEST( ContentGate, OpensNotInTheGapBetweenTwoLinks )
{
    // THE TEST THE SECOND CONDITION EXISTS FOR, written as the chain actually runs.
    //
    //   tick 1: the scene asked for a cloud type           outstanding 1, started 1
    //   tick 2: the pump delivered it; no frame has yet     outstanding 0, started 1   <- THE GAP
    //           had the chance to want the volume it names
    //   frame 2 renders, sees the type, asks for the volume
    //   tick 3: the volume is in flight                     outstanding 1, started 2
    //   tick 4: it landed                                   outstanding 0, started 2
    //   frame 4 renders and asks for nothing
    //   tick 5: quiet twice in a row                        outstanding 0, started 2   -> OPEN
    //
    // A gate watching only `outstanding` opens at tick 2. So does `quiet && frames >= 2`, which is the
    // rule this replaced -- tick 2 is quiet and it is the second tick. Only a STREAK survives the gap:
    // the frame that follows tick 2 is the one that asks, and tick 3 sees it.
    ContentGate gate( ContentState::Loading );
    EXPECT_FALSE( gate.Tick( 1, 1 ) );
    EXPECT_FALSE( gate.Tick( 0, 1 ) ) << "opened in the gap between two links of the chain";
    EXPECT_FALSE( gate.Tick( 1, 2 ) );
    EXPECT_FALSE( gate.Tick( 0, 2 ) );
    EXPECT_TRUE( gate.Loading() );
    EXPECT_TRUE( gate.Tick( 0, 2 ) );
    EXPECT_FALSE( gate.Loading() );
}

TEST( ContentGate, AFrameThatStartedAReadIsNotQuietEvenWithAnEmptyQueue )
{
    // The degenerate version of the case above: a read that both started and finished inside one frame
    // (an already-resident asset re-requested, say) leaves the queue empty and the counter moved. The
    // counter is what catches it.
    ContentGate gate( ContentState::Loading );
    EXPECT_FALSE( gate.Tick( 0, 0 ) );
    EXPECT_FALSE( gate.Tick( 0, 1 ) ) << "a frame that started a read was treated as a quiet one";
    EXPECT_FALSE( gate.Tick( 0, 1 ) );
    EXPECT_TRUE( gate.Loading() );
    EXPECT_TRUE( gate.Tick( 0, 1 ) );
    EXPECT_FALSE( gate.Loading() );
}

TEST( ContentGate, AStreakOfOneIsResetByAnyTrafficAtAll )
{
    // The streak is CONSECUTIVE, not cumulative. A load that goes quiet, asks once more, and goes quiet
    // again must pay the full two quiet ticks over again -- counting quiet ticks anywhere in the wait
    // would let a long busy load open on the strength of two unrelated lulls.
    ContentGate gate( ContentState::Loading );
    EXPECT_FALSE( gate.Tick( 0, 0 ) );   // quiet, streak 1
    EXPECT_FALSE( gate.Tick( 1, 1 ) );   // traffic, streak 0
    EXPECT_FALSE( gate.Tick( 0, 1 ) );   // quiet, streak 1 -- NOT 2
    EXPECT_TRUE( gate.Loading() );
    EXPECT_TRUE( gate.Tick( 0, 1 ) );    // quiet, streak 2
}

TEST( ContentGate, OpensExactlyOnce )
{
    // The caller does its once-only work (start the game, arm the splash, log the cost) on the tick this
    // returns true, and holds no flag of its own -- so a second true would start the game twice.
    ContentGate gate( ContentState::Loading );
    EXPECT_FALSE( gate.Tick( 0, 0 ) );
    EXPECT_TRUE( gate.Tick( 0, 0 ) );
    for ( int i = 0; i < 10; ++i )
        EXPECT_FALSE( gate.Tick( 0, 0 ) ) << "the gate opened a second time on tick " << i;
}

TEST( ContentGate, ALevelSwitchRearmsIt )
{
    // The half that would have been forgotten. A door opened at run time hands over a second world whose
    // clouds and layouts are read on demand exactly like the first one's; a gate that could only close
    // at boot would ship the defect back into the game on the first level change.
    ContentGate gate( ContentState::Loading );
    gate.BeginWorld( 5 ); // the boot's own world, whose reads began at 5
    EXPECT_FALSE( gate.Tick( 0, 5 ) );
    EXPECT_TRUE( gate.Tick( 0, 5 ) );
    ASSERT_FALSE( gate.Loading() );

    gate.BeginWorld( 5 );
    EXPECT_TRUE( gate.Loading() );
    EXPECT_EQ( gate.FramesWaited(), 0u );
    EXPECT_FALSE( gate.Tick( 1, 6 ) );
    EXPECT_FALSE( gate.Tick( 0, 6 ) );
    EXPECT_TRUE( gate.Tick( 0, 6 ) );
    EXPECT_FALSE( gate.Loading() );
}

TEST( ContentGate, BeginWorldTakesTheCounterSoTheFirstFrameIsJudgedAgainstIt )
{
    // `startedCount` is process-wide and monotonic: by the time a second level loads it is already in the
    // hundreds. Judging the first frame of that level against zero would make every first frame look like
    // a frame that started a read, and the gate would be one frame slower for no reason -- or, with a
    // different sign of the same mistake, would never be comparable at all.
    ContentGate gate( ContentState::Ready );
    gate.BeginWorld( 412 );
    EXPECT_FALSE( gate.Tick( 0, 412 ) );
    EXPECT_TRUE( gate.Tick( 0, 412 ) ) << "the wait did not start from the counter BeginWorld was given";
}

TEST( ContentGate, TheWaitIsMeasuredAndFreezesWhenItOpens )
{
    // A number without its own floor is not a measurement, so this asserts the RELATION (the total stops
    // growing once the gate is open) rather than a millisecond count -- the lesson of a test that
    // demanded `< 38 ms` and failed only under a sanitizer.
    ContentGate gate( ContentState::Loading );
    EXPECT_FALSE( gate.Tick( 1, 1 ) );
    std::this_thread::sleep_for( std::chrono::milliseconds( 12 ) );
    EXPECT_FALSE( gate.Tick( 0, 1 ) );
    EXPECT_TRUE( gate.Tick( 0, 1 ) );
    ASSERT_FALSE( gate.Loading() );

    const double settled = gate.ElapsedMs();
    EXPECT_GT( settled, 0.0 );
    std::this_thread::sleep_for( std::chrono::milliseconds( 12 ) );
    EXPECT_DOUBLE_EQ( gate.ElapsedMs(), settled ) << "the wait kept running after the gate opened";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
