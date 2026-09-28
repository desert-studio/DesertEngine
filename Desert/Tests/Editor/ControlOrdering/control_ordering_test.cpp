// A REPLY MUST NOT OVERTAKE THE FRAME THAT PROVES IT.
//
// The control channel exists so that an agent can drive the editor and photograph the result. That is only
// worth anything if the photograph is of the state AFTER the command: a snapshot that overtakes its own
// action shows the state BEFORE it and is completely convincing to look at. It is the same class as "the
// first render after a shader edit is not evidence" -- a picture that is real, of the wrong moment.
//
// THE OBVIOUS RULE IS THE WRONG RULE, which is the thing this suite is really about. "Execute at the top of
// the update, answer after that frame" sounds exact. It is not: EditorLayer runs a block of DEFERRED work
// each frame -- document closes, asset opens, scene loads, leaving Play -- and several commands only land in
// a queue there. A reply one frame later would be right often enough to be trusted, and wrong exactly when
// something interesting had happened.
//
// So the gate is conditioned on QUIESCENCE and not counted in frames, and these are the relations that say
// the condition is real rather than decorative:
//
//   1. ORDER. A frame that STARTED before the command cannot discharge the gate, whatever its quiescence.
//   2. SETTLING. A frame drawn while ANY kind of work was outstanding cannot discharge it either.
//   3. ONCE. The gate discharges at most once per arming: one request, one reply.
//   4. NO SILENT FOREVER. An editor that never settles produces a REFUSAL naming what stayed outstanding.
//   5. THE CENSUS IS CLOSED. Every kind of outstanding work has a name, checked by the compiler, so a new
//      deferred queue cannot be added without teaching the gate about it. That is the failure that would
//      not announce itself: a missing entry opens the gate one frame early and nothing says anything.
//
// Editor/Core/Control/ControlPipeline.hpp is pure -- no socket, no ImGui, no renderer, no clock -- which is
// the only reason any of this can be asserted: EditorLayer.cpp, where the flags actually live, is compiled
// by no suite at all (scripts/CI/UnreachedSources.sh).

#include <Editor/Core/Control/ControlPipeline.hpp>

#include <gtest/gtest.h>

#include <string>

using Desert::Editor::Control::DescribeReadinessTimeout;
using Desert::Editor::Control::DescribeSettleTimeout;
using Desert::Editor::Control::EditorQuiescence;
using Desert::Editor::Control::FrameGate;
using Desert::Editor::Control::GateSubject;
using Desert::Editor::Control::GateVerdict;
using Desert::Editor::Control::kPendingWorkNames;
using Desert::Editor::Control::PendingWork;

namespace
{
    EditorQuiescence Settled()
    {
        return EditorQuiescence{};
    }

    EditorQuiescence Busy( PendingWork kind )
    {
        EditorQuiescence quiescence;
        quiescence.Set( kind, true );
        return quiescence;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 5. The census is closed.
// ---------------------------------------------------------------------------------------------------

// EVERY kind of outstanding work is named, and the count is checked where it cannot be forgotten.
//
// The static_assert in the header is the real enforcement -- adding an enumerator without a name does not
// COMPILE. It is restated here as a runtime expression for the reason this project restates compile-time
// rules generally: a rule only the compiler can see is a rule no suite can show going red, and "it did not
// build" is not a test result anybody can read a month later.
TEST( ControlOrdering, EveryKindOfPendingWorkHasAName )
{
    ASSERT_EQ( std::size( kPendingWorkNames ), static_cast<std::size_t>( PendingWork::Count ) );

    for ( std::size_t i = 0; i < std::size( kPendingWorkNames ); ++i )
    {
        ASSERT_NE( kPendingWorkNames[i], nullptr );
        EXPECT_FALSE( std::string( kPendingWorkNames[i] ).empty() )
             << "PendingWork #" << i << " has an empty name; a refusal that quotes it would say nothing.";
    }
}

// Settled() is a loop over the census, not a chain of `&&`. The relation that says so: EVERY kind, on its
// own, unsettles the editor. A chain that omitted one would pass a test that only tried a couple of them.
TEST( ControlOrdering, AnySingleOutstandingKindUnsettlesTheEditor )
{
    EXPECT_TRUE( Settled().Settled() ) << "nothing outstanding must read as settled";

    for ( std::size_t i = 0; i < static_cast<std::size_t>( PendingWork::Count ); ++i )
    {
        const auto kind = static_cast<PendingWork>( i );
        EXPECT_FALSE( Busy( kind ).Settled() )
             << "'" << kPendingWorkNames[i] << "' was outstanding and the editor still reported settled";
    }
}

// A refusal has to name what is outstanding, or the client is told "it did not settle" and has nothing to
// act on. Describe() is empty exactly when Settled(), and names the kind otherwise.
TEST( ControlOrdering, DescribeNamesExactlyWhatIsOutstanding )
{
    EXPECT_TRUE( Settled().Describe().empty() );

    for ( std::size_t i = 0; i < static_cast<std::size_t>( PendingWork::Count ); ++i )
    {
        const auto        kind        = static_cast<PendingWork>( i );
        const std::string description = Busy( kind ).Describe();
        EXPECT_EQ( description, kPendingWorkNames[i] );
    }

    EditorQuiescence two;
    two.Set( PendingWork::DocumentCloses, true );
    two.Set( PendingWork::AssetOpens, true );
    const std::string both = two.Describe();
    EXPECT_NE( both.find( kPendingWorkNames[static_cast<std::size_t>( PendingWork::DocumentCloses )] ),
               std::string::npos );
    EXPECT_NE( both.find( kPendingWorkNames[static_cast<std::size_t>( PendingWork::AssetOpens )] ),
               std::string::npos );
}

// LS-10b: a background run (New Landscape's Create) answers its command at once. It holds no reply — a frame
// drawn while it runs discharges the gate — yet the editor is not idle, and `state` names the run, which is
// what a client polls before photographing the result.
TEST( ControlOrdering, BackgroundWorkHoldsNoReplyButIsReportedUntilDone )
{
    using Desert::Editor::Control::BackgroundWork;
    using Desert::Editor::Control::kBackgroundWorkNames;
    for ( std::size_t i = 0; i < static_cast<std::size_t>( BackgroundWork::Count ); ++i )
    {
        EditorQuiescence running;
        running.Set( static_cast<BackgroundWork>( i ), true );
        EXPECT_TRUE( running.Settled() ) << kBackgroundWorkNames[i];
        EXPECT_TRUE( running.Describe().empty() ) << "a refusal must not blame a run nobody waits for";
        EXPECT_FALSE( running.Idle() ) << kBackgroundWorkNames[i];
        EXPECT_EQ( running.DescribeBackground(), kBackgroundWorkNames[i] );

        FrameGate gate;
        gate.ArmAfterExecution( 0 );
        EXPECT_EQ( gate.ObserveFramePresented( 0, running ), GateVerdict::Discharged ) << kBackgroundWorkNames[i];
    }
    EXPECT_TRUE( Settled().Idle() );
    EXPECT_TRUE( Settled().DescribeBackground().empty() );
    // Foreground work still unsettles, background or not.
    EditorQuiescence both = Busy( PendingWork::SceneLoad );
    both.Set( BackgroundWork::LandscapeGenerate, true );
    EXPECT_FALSE( both.Settled() );
    EXPECT_FALSE( both.Idle() );
}

// ---------------------------------------------------------------------------------------------------
// 1-3. Order, settling, and discharging once.
// ---------------------------------------------------------------------------------------------------

TEST( ControlOrdering, AnUnarmedGateIsIdleAndAnswersNothing )
{
    FrameGate gate;
    EXPECT_FALSE( gate.IsArmed() );
    EXPECT_EQ( gate.ObserveFramePresented( 7, Settled() ), GateVerdict::Idle );
}

// ORDER. A frame that was already being built when the command ran cannot show it. This is separate from
// the settling condition on purpose: the two prevent different failures, and one condition covering both
// would be right for the wrong reason.
TEST( ControlOrdering, AFrameOlderThanTheCommandCannotDischargeTheGate )
{
    FrameGate gate;
    gate.ArmAfterExecution( 10 );

    EXPECT_EQ( gate.ObserveFramePresented( 9, Settled() ), GateVerdict::Waiting )
         << "a frame that started before the command was executed cannot reflect it, settled or not";
    EXPECT_TRUE( gate.IsArmed() );

    EXPECT_EQ( gate.ObserveFramePresented( 10, Settled() ), GateVerdict::Discharged );
}

// SETTLING. The frame is new enough, and still does not count, because the command's own work was queued.
// This is the case the naive "one frame later" rule gets wrong: opening a document goes through a queue,
// so the frame the command was issued on is drawn before the window exists.
TEST( ControlOrdering, AFrameDrawnWithWorkOutstandingCannotDischargeTheGate )
{
    FrameGate gate;
    gate.ArmAfterExecution( 3 );

    EXPECT_EQ( gate.ObserveFramePresented( 3, Busy( PendingWork::AssetOpens ) ), GateVerdict::Waiting );
    EXPECT_EQ( gate.ObserveFramePresented( 4, Busy( PendingWork::OpenRefusal ) ), GateVerdict::Waiting );
    EXPECT_TRUE( gate.IsArmed() );

    // The queues drained and the dialog is up: THIS frame is the one that shows what was asked for.
    EXPECT_EQ( gate.ObserveFramePresented( 5, Settled() ), GateVerdict::Discharged );
}

// ONCE. One request gets one reply. A gate that could discharge twice would answer a request the client
// already considers finished, and the second answer would arrive as a reply to the NEXT one.
TEST( ControlOrdering, TheGateDischargesAtMostOncePerArming )
{
    FrameGate gate;
    gate.ArmAfterExecution( 0 );

    EXPECT_EQ( gate.ObserveFramePresented( 0, Settled() ), GateVerdict::Discharged );
    EXPECT_FALSE( gate.IsArmed() );
    EXPECT_EQ( gate.ObserveFramePresented( 1, Settled() ), GateVerdict::Idle );
    EXPECT_EQ( gate.ObserveFramePresented( 2, Settled() ), GateVerdict::Idle );

    gate.ArmAfterExecution( 3 );
    EXPECT_EQ( gate.ObserveFramePresented( 3, Settled() ), GateVerdict::Discharged );
}

// ---------------------------------------------------------------------------------------------------
// 4. No silent forever.
// ---------------------------------------------------------------------------------------------------

// An editor that never settles must ANSWER. Holding the client until it gives up is the failure mode this
// whole channel replaces -- a run with nobody watching, waiting on something that will not happen.
TEST( ControlOrdering, AnEditorThatNeverSettlesTimesOutRatherThanWaitingForever )
{
    FrameGate gate;
    gate.ArmAfterExecution( 0 );

    const EditorQuiescence stuck = Busy( PendingWork::SceneLoad );

    for ( uint32_t frame = 0; frame + 1 < FrameGate::kMaxSettleFrames; ++frame )
    {
        ASSERT_EQ( gate.ObserveFramePresented( frame, stuck ), GateVerdict::Waiting )
             << "timed out early, at frame " << frame;
    }

    EXPECT_EQ( gate.ObserveFramePresented( FrameGate::kMaxSettleFrames - 1, stuck ), GateVerdict::TimedOut );
    EXPECT_FALSE( gate.IsArmed() ) << "a timed-out gate must not go on answering";
}

// The refusal a timeout produces names what stayed outstanding and how long it waited. "The editor did not
// settle" alone is a message nobody can do anything with.
TEST( ControlOrdering, ASettleTimeoutNamesWhatStayedOutstanding )
{
    const std::string message = DescribeSettleTimeout( Busy( PendingWork::DocumentCloses ), 240 );

    EXPECT_NE( message.find( "240" ), std::string::npos );
    EXPECT_NE( message.find( kPendingWorkNames[static_cast<std::size_t>( PendingWork::DocumentCloses )] ),
               std::string::npos );
}

// A timeout reported against a SETTLED census is the channel contradicting itself -- the gate refused on
// the grounds that work was outstanding, and the census says none was. That must read as a defect in the
// channel rather than as a fact about the editor, because it is one.
TEST( ControlOrdering, ATimeoutWithNothingOutstandingBlamesTheChannel )
{
    const std::string message = DescribeSettleTimeout( Settled(), 240 );
    EXPECT_NE( message.find( "defect in the channel" ), std::string::npos );
}

// Abandoning is not discharging. When the client goes away or the editor closes, the gate must not report
// the frame as proof of anything -- there is no reply to send and no frame that proved it.
TEST( ControlOrdering, DisarmingIsNotDischarging )
{
    FrameGate gate;
    gate.ArmAfterExecution( 1 );
    gate.Disarm();

    EXPECT_FALSE( gate.IsArmed() );
    EXPECT_EQ( gate.ObserveFramePresented( 1, Settled() ), GateVerdict::Idle );
    EXPECT_EQ( gate.FramesWaited(), 0u );
}

// ---------------------------------------------------------------------------------------------------
// 6. THE OTHER END OF THE SAME WAIT: an answer must not overtake the editor COMING UP.
//
// The gate began holding one thing -- a reply, until the frame that proves its command. A6-1 gave it the
// mirror case, and it is the same question of the same census: a request that arrived BEFORE the editor had
// read the project used to be answered anyway, from a state that was half built. Measured on this
// repository's own project, the palette's `Open` group goes 0 -> 106 -> 130 as five separate startup stages
// fill the asset cache, and the 106-entry answer -- every material, not one of the twenty-four cloud assets
// -- is a SUCCESSFUL reply that stands for 3.3 seconds of every boot. Nothing in it says which it is.
//
// The relations that make the second wait real rather than decorative:
//
//   7. TWO SUBJECTS, ONE MECHANISM. The gate says WHICH wait it is serving, because a discharge means two
//      different things and the caller must not have to guess.
//   8. READINESS IS OBSERVED, NOT INFERRED. A frame drawn while the staged startup was running cannot
//      release a parked request, exactly as it cannot release a reply.
//   9. NO SILENT FOREVER, AGAIN -- and the refusal says the request NEVER RAN, which is a different fact
//      from "it ran and the reply cannot vouch for the picture".
// ---------------------------------------------------------------------------------------------------

// The gate must be able to say which of the two things it is holding. A caller that could not ask would
// have to infer it from somewhere else, and "somewhere else" is a second answer to one question.
TEST( ControlOrdering, TheGateSaysWhichOfTheTwoWaitsItIsServing )
{
    FrameGate gate;
    EXPECT_EQ( gate.Holding(), GateSubject::Nothing );

    gate.ArmForReadiness( 3 );
    EXPECT_EQ( gate.Holding(), GateSubject::Readiness );
    EXPECT_TRUE( gate.IsArmed() );

    gate.ArmAfterExecution( 3 );
    EXPECT_EQ( gate.Holding(), GateSubject::Effect );
    EXPECT_TRUE( gate.IsArmed() );

    gate.Disarm();
    EXPECT_EQ( gate.Holding(), GateSubject::Nothing );
    EXPECT_FALSE( gate.IsArmed() );
}

// A discharge clears the subject. That is what lets EditorLayer hold ONE state -- "a request in flight with
// an idle gate has been released and has not run yet" -- instead of a second flag beside the gate saying
// the same thing, which is the shape that drifts.
TEST( ControlOrdering, ADischargedReadinessWaitLeavesTheGateHoldingNothing )
{
    FrameGate gate;
    gate.ArmForReadiness( 0 );

    EXPECT_EQ( gate.ObserveFramePresented( 0, Settled() ), GateVerdict::Discharged );
    EXPECT_EQ( gate.Holding(), GateSubject::Nothing );
}

// THE RULE THIS WHOLE HALF EXISTS FOR. A frame drawn while the staged startup was still running proves
// nothing about a project the editor has not read, so it cannot release a request that is waiting to be
// answered about that project. Asserted for EVERY kind of outstanding work rather than for the startup
// alone: a scene load in flight makes an answer about the scene's entities just as wrong.
TEST( ControlOrdering, AFrameDrawnBeforeTheEditorIsUpCannotReleaseAParkedRequest )
{
    for ( std::size_t i = 0; i < static_cast<std::size_t>( PendingWork::Count ); ++i )
    {
        FrameGate gate;
        gate.ArmForReadiness( 0 );

        const EditorQuiescence busy = Busy( static_cast<PendingWork>( i ) );
        EXPECT_EQ( gate.ObserveFramePresented( 0, busy ), GateVerdict::Waiting ) << kPendingWorkNames[i];
        EXPECT_EQ( gate.Holding(), GateSubject::Readiness ) << kPendingWorkNames[i];

        // ...and the very next settled frame does release it, so the wait is a wait and not a refusal.
        EXPECT_EQ( gate.ObserveFramePresented( 1, Settled() ), GateVerdict::Discharged ) << kPendingWorkNames[i];
    }
}

// The staged boot runs ONE STAGE PER FRAME, so a boot is as many frames as it has stages -- eight here --
// whether those frames cost three seconds or five minutes. This is the assertion that says the frame budget
// is the right unit: a whole boot's worth of frames, thirty times over, still leaves the request parked
// rather than refused.
TEST( ControlOrdering, AReadinessWaitOutlastsAWholeBootWorthOfFrames )
{
    FrameGate gate;
    gate.ArmForReadiness( 0 );

    const EditorQuiescence booting = Busy( PendingWork::StartupLoading );
    for ( uint32_t frame = 0; frame < 200; ++frame )
        ASSERT_EQ( gate.ObserveFramePresented( frame, booting ), GateVerdict::Waiting ) << "gave up at " << frame;

    EXPECT_EQ( gate.ObserveFramePresented( 200, Settled() ), GateVerdict::Discharged );
}

// An editor that never comes up refuses, and the refusal is NOT the settle timeout's. The two are at
// opposite ends of the request: this one means the command never ran, so nothing in the editor changed; the
// other means it ran and the reply cannot vouch for the picture. A client told the wrong one either retries
// something that already happened or gives up on something that never did.
TEST( ControlOrdering, AnEditorThatNeverComesUpRefusesSayingTheRequestNeverRan )
{
    FrameGate gate;
    gate.ArmForReadiness( 0 );

    const EditorQuiescence stuck = Busy( PendingWork::StartupLoading );
    for ( uint32_t frame = 0; frame + 1 < FrameGate::kMaxSettleFrames; ++frame )
        ASSERT_EQ( gate.ObserveFramePresented( frame, stuck ), GateVerdict::Waiting );

    EXPECT_EQ( gate.ObserveFramePresented( FrameGate::kMaxSettleFrames - 1, stuck ), GateVerdict::TimedOut );
    EXPECT_EQ( gate.Holding(), GateSubject::Nothing );

    const std::string message = DescribeReadinessTimeout( stuck, FrameGate::kMaxSettleFrames );
    EXPECT_NE( message.find( "NOT run" ), std::string::npos );
    EXPECT_NE( message.find( kPendingWorkNames[static_cast<std::size_t>( PendingWork::StartupLoading )] ),
               std::string::npos );
    // The client is told where the same census can be read at any moment, so "wait and see" is a thing it
    // can actually do rather than a thing it has to guess a duration for.
    EXPECT_NE( message.find( "state" ), std::string::npos );

    // ...and it is not the OTHER refusal wearing a new coat. Two messages that had drifted into one would
    // be exactly the confusion this pair exists to prevent.
    EXPECT_NE( message, DescribeSettleTimeout( stuck, FrameGate::kMaxSettleFrames ) );
}

// Same self-contradiction check the settle timeout gets: a readiness refusal against a settled census means
// the gate and the census disagree, which is a defect in the channel and not a fact about the editor.
TEST( ControlOrdering, AReadinessTimeoutWithNothingOutstandingBlamesTheChannel )
{
    EXPECT_NE( DescribeReadinessTimeout( Settled(), 240 ).find( "defect in the channel" ), std::string::npos );
}

// ---------------------------------------------------------------------------------------------------
// 7. The peek and the verdict agree.
// ---------------------------------------------------------------------------------------------------

// A capture of the composited frame has to be RECORDED while that frame is still being built: a swapchain
// image may only be touched between its acquire and its present, and a copy issued after the present is a
// spec violation the driver here tolerates in silence. So the editor asks, before it submits, whether this
// frame is the one the reply is waiting for.
//
// THAT MAKES THE TWO ANSWERS A RELATION, and it is the relation this test exists for: a frame WouldDischarge
// says yes to must be a frame ObserveFramePresented then discharges. Disagreement is not a cosmetic fault —
// it means either a capture recorded for a frame that did not count, or a discharged reply carrying no
// capture at all, and the second one answers "ok" with no PNG behind it.
TEST( ControlOrdering, ThePeekAgreesWithTheVerdictOnEveryFrameItIsAskedAbout )
{
    for ( const auto& outstanding : { PendingWork::AssetOpens, PendingWork::DocumentCloses } )
    {
        for ( uint64_t executedOn : { uint64_t{ 0 }, uint64_t{ 5 } } )
        {
            for ( uint64_t frame : { uint64_t{ 0 }, uint64_t{ 4 }, uint64_t{ 5 }, uint64_t{ 9 } } )
            {
                for ( const EditorQuiescence& quiescence : { Settled(), Busy( outstanding ) } )
                {
                    FrameGate gate;
                    gate.ArmAfterExecution( executedOn );

                    const bool peeked = gate.WouldDischarge( frame, quiescence );
                    const bool judged = gate.ObserveFramePresented( frame, quiescence ) == GateVerdict::Discharged;

                    EXPECT_EQ( peeked, judged ) << "armed on " << executedOn << ", frame " << frame << ", settled "
                                                << quiescence.Settled();
                }
            }
        }
    }
}

// An unarmed gate never claims a frame. Recording a capture for one would allocate a full frame of device
// memory that nothing would ever collect.
TEST( ControlOrdering, AnUnarmedGateNeverClaimsAFrame )
{
    FrameGate gate;
    EXPECT_FALSE( gate.WouldDischarge( 0, Settled() ) );
    EXPECT_FALSE( gate.WouldDischarge( 1000, Settled() ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
