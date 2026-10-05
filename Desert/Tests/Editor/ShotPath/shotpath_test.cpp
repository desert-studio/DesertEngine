#include <gtest/gtest.h>

#include <Editor/Core/ShotOptions.hpp>

#include <cmath>

using namespace Desert::Editor;

namespace
{
    // The angle between two directions, in degrees. The interpolator's whole job is to divide this
    // evenly, so every assertion about it is an assertion about this number.
    float AngleDegrees( const glm::vec3& a, const glm::vec3& b )
    {
        const float c = glm::clamp( glm::dot( glm::normalize( a ), glm::normalize( b ) ), -1.0f, 1.0f );
        return glm::degrees( std::acos( c ) );
    }
} // namespace

// ── The relation the static-shot evidence rests on ───────────────────────────────────────────────
//
// Every shot in the repository so far was taken with --camera/--look and no path. Such a shot IS
// bit-reproducible from the same binary (measured: three runs, one md5) — but only because gameplay
// time did not advance, so there was no clock in the picture; under `--play` a fixed step is what
// keeps that true. Either way the POSE is the thing standing between the old commands and a different
// picture. The claim is therefore not "close": it is EQ, on the exact float, at the exact parameter
// the layer uses.
TEST( ShotPath, WithoutMotionFlagsThePoseIsTheStaticOneExactly )
{
    ShotOptions shot;
    shot.Position = glm::vec3( 1234.5f, -678.25f, 90.125f );
    shot.Forward  = glm::vec3( 0.0f, 0.30f, -1.0f ); // deliberately NOT unit length, as --look allows
    shot.Frames   = 90;

    EXPECT_FALSE( shot.HasMotion() );

    // The layer places the pose once, at the parameter of frame 0.
    const ShotCamera placed = shot.CameraAt( shot.Parameter( 0 ) );
    EXPECT_EQ( placed.Position, shot.Position );
    EXPECT_EQ( placed.Forward, shot.Forward );

    // And would get the same answer at any other frame, so the guard in the layer is an optimization
    // rather than the thing that makes the identity true.
    for ( int frame : { 1, 45, 89 } )
    {
        const ShotCamera later = shot.CameraAt( shot.Parameter( frame ) );
        EXPECT_EQ( later.Position, shot.Position );
        EXPECT_EQ( later.Forward, shot.Forward );
    }
}

TEST( ShotPath, ParameterSpansZeroToOneAcrossTheRenderedFrames )
{
    ShotOptions shot;
    shot.Frames = 90;

    EXPECT_FLOAT_EQ( shot.Parameter( 0 ), 0.0f );
    EXPECT_FLOAT_EQ( shot.Parameter( 89 ), 1.0f );
    EXPECT_NEAR( shot.Parameter( 45 ), 45.0f / 89.0f, 1e-6f );

    // Out of range is clamped, not extrapolated: a frame index past the end must not fling the camera
    // beyond the endpoint the command line named.
    EXPECT_FLOAT_EQ( shot.Parameter( -5 ), 0.0f );
    EXPECT_FLOAT_EQ( shot.Parameter( 200 ), 1.0f );

    // A one-frame shot has no path to divide; it sits at the start.
    ShotOptions single;
    single.Frames = 1;
    EXPECT_FLOAT_EQ( single.Parameter( 0 ), 0.0f );
}

TEST( ShotPath, EndpointsAreReproducedExactly )
{
    const glm::vec3 a( 0.0f, 0.30f, -1.0f );
    const glm::vec3 b( 1.0f, 0.10f, 0.0f );

    EXPECT_EQ( ShotSlerpDirection( a, b, 0.0f ), a );
    EXPECT_EQ( ShotSlerpDirection( a, b, 1.0f ), b );

    ShotOptions shot;
    shot.Position      = glm::vec3( 0.0f, 200.0f, 0.0f );
    shot.Forward       = a;
    shot.HasPositionTo = true;
    shot.PositionTo    = glm::vec3( 5000.0f, 200.0f, 0.0f );
    shot.HasForwardTo  = true;
    shot.ForwardTo     = b;

    EXPECT_EQ( shot.CameraAt( 0.0f ).Position, shot.Position );
    EXPECT_EQ( shot.CameraAt( 0.0f ).Forward, shot.Forward );
    EXPECT_EQ( shot.CameraAt( 1.0f ).Position, shot.PositionTo );
    EXPECT_EQ( shot.CameraAt( 1.0f ).Forward, shot.ForwardTo );
}

// The property that makes the frame-to-frame metric mean anything: equal steps in t are equal steps
// in ANGLE. A component-wise lerp fails this by ~4 degrees on a 90 degree turn, and that error would
// appear in the measurement as a hump in the middle of every pan.
TEST( ShotPath, TurnRateIsConstantAlongThePath )
{
    const glm::vec3 from( 0.0f, 0.0f, -1.0f );
    const glm::vec3 to( 1.0f, 0.0f, 0.0f ); // 90 degrees of yaw

    const float total = AngleDegrees( from, to );
    EXPECT_NEAR( total, 90.0f, 1e-3f );

    const int steps = 16;
    for ( int i = 1; i <= steps; ++i )
    {
        const float t = static_cast<float>( i ) / static_cast<float>( steps );
        EXPECT_NEAR( AngleDegrees( from, ShotSlerpDirection( from, to, t ) ), total * t, 1e-2f );
    }

    // Consecutive samples are the same angular size, which is the form the metric actually depends on.
    const float first =
         AngleDegrees( ShotSlerpDirection( from, to, 0.0f ), ShotSlerpDirection( from, to, 1.0f / steps ) );
    for ( int i = 1; i < steps; ++i )
    {
        const float t0   = static_cast<float>( i ) / static_cast<float>( steps );
        const float t1   = static_cast<float>( i + 1 ) / static_cast<float>( steps );
        const float step = AngleDegrees( ShotSlerpDirection( from, to, t0 ), ShotSlerpDirection( from, to, t1 ) );
        EXPECT_NEAR( step, first, 1e-2f );
    }
}

// Between two DISTINCT directions the interior of the arc is unit length. The endpoints and the
// coincident case are not, deliberately — they hand back the caller's own float (see above), and the
// consumer normalizes regardless.
TEST( ShotPath, TheInteriorOfAnArcIsUnitLength )
{
    const glm::vec3 from( 0.0f, 0.30f, -1.0f ); // not unit length on entry
    const glm::vec3 to( -1.0f, 0.90f, 0.20f );

    for ( int i = 1; i < 16; ++i )
    {
        const float t = static_cast<float>( i ) / 16.0f;
        EXPECT_NEAR( glm::length( ShotSlerpDirection( from, to, t ) ), 1.0f, 1e-4f );
    }
}

// A half turn has no shortest arc — every path round is the same length — so the cross product that
// would name the axis is the zero vector. Without the explicit branch this is a NaN pose, i.e. a
// camera that renders nothing, on the most obvious command anyone would type for a whip pan.
TEST( ShotPath, ExactlyOpposedDirectionsStillTurn )
{
    const glm::vec3 from( 0.0f, 0.0f, -1.0f );
    const glm::vec3 to( 0.0f, 0.0f, 1.0f );

    const glm::vec3 mid = ShotSlerpDirection( from, to, 0.5f );
    EXPECT_TRUE( std::isfinite( mid.x ) && std::isfinite( mid.y ) && std::isfinite( mid.z ) );
    EXPECT_NEAR( glm::length( mid ), 1.0f, 1e-4f );
    EXPECT_NEAR( AngleDegrees( from, mid ), 90.0f, 1e-2f );
    EXPECT_NEAR( AngleDegrees( ShotSlerpDirection( from, to, 0.25f ), from ), 45.0f, 1e-2f );

    // The same must hold when the opposed pair runs along the axis the fallback perpendicular is
    // chosen from, which is where a naive "cross with X" degenerates a second time.
    const glm::vec3 alongX( 1.0f, 0.0f, 0.0f );
    const glm::vec3 midX = ShotSlerpDirection( alongX, -alongX, 0.5f );
    EXPECT_NEAR( glm::length( midX ), 1.0f, 1e-4f );
    EXPECT_NEAR( AngleDegrees( alongX, midX ), 90.0f, 1e-2f );
}

// --camera-to without --look-to must hold the aim; --look-to without --camera-to must hold the eye.
// Getting this wrong turns a pure translation (the disocclusion case) into a translation plus a pan,
// which is exactly the confound the disocclusion measurement cannot survive.
TEST( ShotPath, OneEndpointGivenLeavesTheOtherAlone )
{
    ShotOptions translate;
    translate.Position      = glm::vec3( 0.0f, 200.0f, 0.0f );
    translate.Forward       = glm::vec3( 0.0f, 0.10f, -1.0f );
    translate.HasPositionTo = true;
    translate.PositionTo    = glm::vec3( 60000.0f, 200.0f, 0.0f );

    EXPECT_TRUE( translate.HasMotion() );
    for ( float t : { 0.0f, 0.5f, 1.0f } )
        EXPECT_EQ( translate.CameraAt( t ).Forward, translate.Forward );
    EXPECT_EQ( translate.CameraAt( 1.0f ).Position, translate.PositionTo );

    ShotOptions pan;
    pan.Position     = glm::vec3( 0.0f, 200.0f, 0.0f );
    pan.Forward      = glm::vec3( 0.0f, 0.10f, -1.0f );
    pan.HasForwardTo = true;
    pan.ForwardTo    = glm::vec3( 1.0f, 0.10f, 0.0f );

    EXPECT_TRUE( pan.HasMotion() );
    for ( float t : { 0.0f, 0.5f, 1.0f } )
        EXPECT_EQ( pan.CameraAt( t ).Position, pan.Position );
    EXPECT_EQ( pan.CameraAt( 1.0f ).Forward, pan.ForwardTo );
}

TEST( ShotPath, PositionIsInterpolatedLinearly )
{
    ShotOptions shot;
    shot.Position      = glm::vec3( -1000.0f, 200.0f, 500.0f );
    shot.HasPositionTo = true;
    shot.PositionTo    = glm::vec3( 3000.0f, 800.0f, -1500.0f );

    const glm::vec3 mid = shot.CameraAt( 0.5f ).Position;
    EXPECT_NEAR( mid.x, 1000.0f, 1e-2f );
    EXPECT_NEAR( mid.y, 500.0f, 1e-2f );
    EXPECT_NEAR( mid.z, -500.0f, 1e-2f );
}

// Active() is what decides whether the editor is headless at all. A sequence with no designated last
// frame is a legitimate run — a motion study wants the frames, not one of them.
TEST( ShotPath, EitherOutputActivatesHeadlessCapture )
{
    ShotOptions none;
    EXPECT_FALSE( none.Active() );

    ShotOptions still;
    still.Output = "/tmp/out.png";
    EXPECT_TRUE( still.Active() );

    ShotOptions sequence;
    sequence.Sequence = "/tmp/seq";
    EXPECT_TRUE( sequence.Active() );
}

// ── --play: the relations the moving-world capture rests on ──────────────────────────────────────
//
// The flag lets gameplay time run during a capture, which is how anything driven by the world moving
// (cloud advection, particles, foliage, animation, physics) can reach a verification frame at all. Three
// separate things have to hold for it to be a measuring instrument rather than a demo, and each is a
// relation between two places that would otherwise only be checked by someone remembering.

// 1. THE DEFAULT DOES NOT MOVE. Every frame and every md5 in Docs/Clouds/CALIBRATION.md was captured on a
//    frozen world; a flag that leaked into a plain `--shot` would invalidate the whole corpus at once. The
//    frame evidence for this is six protocol points byte-for-byte, but the property itself is one line of
//    logic and is asserted here so a future edit cannot pass tests while breaking it.
TEST( ShotPath, GameplayTimeIsOffUnlessAskedFor )
{
    ShotOptions shot;
    shot.Output = "/tmp/out.png";
    EXPECT_FALSE( shot.Play );
    EXPECT_FALSE( shot.PlayActive() );
    EXPECT_FLOAT_EQ( shot.SimulatedSeconds( 90 ), 0.0f );

    // And the timestep is the measured one, on the exact float — not "close to", because a capture from
    // before this flag existed has to be the same capture.
    for ( float wall : { 0.0f, 1.0f / 60.0f, 0.013913f, 0.5f } )
        EXPECT_EQ( shot.FrameSeconds( wall, true ), wall );
}

// 2. `--play` OUTSIDE A CAPTURE IS NOT A MODE. The editor's Play button is that. Honouring the flag in a
//    headful session would be a second, invisible way into Play that the toolbar does not know about.
TEST( ShotPath, PlayNeedsACaptureToMeanAnything )
{
    ShotOptions headful;
    headful.Play = true;
    EXPECT_FALSE( headful.Active() );
    EXPECT_FALSE( headful.PlayActive() );
    EXPECT_EQ( headful.FrameSeconds( 0.031f, true ), 0.031f );
    EXPECT_FLOAT_EQ( headful.SimulatedSeconds( 1800 ), 0.0f );

    ShotOptions capture;
    capture.Play   = true;
    capture.Output = "/tmp/out.png";
    EXPECT_TRUE( capture.PlayActive() );

    // A sequence-only run is a capture too, and a motion study is exactly the run that wants this flag.
    ShotOptions sequence;
    sequence.Play     = true;
    sequence.Sequence = "/tmp/seq";
    EXPECT_TRUE( sequence.PlayActive() );
}

// 3. THE STEP DOES NOT COME FROM A CLOCK. This is the whole difference between a tool and a curiosity: the
//    wall-clock step the editor normally runs on makes the simulated duration a function of how fast the
//    machine drew, so two runs of one command would put the wind in different places and no capture could
//    be compared with another.
TEST( ShotPath, UnderPlayTheStepIsFixedAndIgnoresTheWallClock )
{
    ShotOptions shot;
    shot.Play   = true;
    shot.Output = "/tmp/out.png";

    // Every wall-clock value a loaded machine could hand us — a fast frame, a slow one, a stall, a
    // zero-length one — produces the SAME step.
    for ( float wall : { 0.0f, 0.001f, 1.0f / 60.0f, 0.25f, 3.0f } )
        EXPECT_EQ( shot.FrameSeconds( wall, true ), ShotOptions::PlayStepSeconds );

    // And the step is 60 Hz specifically. Not a taste: every "N seconds" quoted in a report is
    // `--shot-frames` divided by this number, so moving it silently rewrites the recorded measurements.
    EXPECT_EQ( ShotOptions::PlayStepSeconds, 1.0f / 60.0f );
}

// 3b. BEFORE THE COUNT STARTS, PLAY TIME STANDS STILL. The number of start-up frames (window layout, the
//     reveal, asynchronous loads) differs from run to run; a world that advanced one fixed step per such
//     frame puts the wind and every particle somewhere else on each run. Outside `--play` the flag is inert:
//     the editor's own clock is never held.
TEST( ShotPath, BeforeTheCountStartsPlayTimeStandsStill )
{
    ShotOptions shot;
    shot.Play   = true;
    shot.Output = "/tmp/out.png";
    for ( const float wall : { 0.0f, 1.0f / 60.0f, 0.25f } )
        EXPECT_EQ( shot.FrameSeconds( wall, false ), 0.0f );

    ShotOptions still;
    still.Output = "/tmp/out.png";
    for ( const float wall : { 0.0f, 0.013913f, 0.5f } )
        EXPECT_EQ( still.FrameSeconds( wall, false ), wall );
}

// The relation a report quotes: frames and seconds are the same statement. "Take the shot after 30
// seconds" is `--play --shot-frames 1800`, and nothing else in the command line decides it.
TEST( ShotPath, SimulatedSecondsIsFramesTimesTheStep )
{
    ShotOptions shot;
    shot.Play   = true;
    shot.Output = "/tmp/out.png";

    EXPECT_NEAR( shot.SimulatedSeconds( 1800 ), 30.0f, 1e-4f );
    EXPECT_NEAR( shot.SimulatedSeconds( 90 ), 1.5f, 1e-5f );
    EXPECT_NEAR( shot.SimulatedSeconds( 60 ), 1.0f, 1e-6f );

    // A capture with no frames is no time, rather than a negative amount of it.
    EXPECT_FLOAT_EQ( shot.SimulatedSeconds( 0 ), 0.0f );
    EXPECT_FLOAT_EQ( shot.SimulatedSeconds( -10 ), 0.0f );
}

// The two halves of the shot are INDEPENDENT: `--play` moves the world, the path flags move the camera,
// and neither may quietly become the other. A still `--play` capture is the one this task exists for —
// the camera fixed, so that whatever moves in the frame is the world and nothing else.
TEST( ShotPath, PlayDoesNotDisturbTheCameraPath )
{
    ShotOptions shot;
    shot.Output   = "/tmp/out.png";
    shot.Position = glm::vec3( 0.0f, 200.0f, 0.0f );
    shot.Forward  = glm::vec3( 0.0f, 0.45f, -1.0f );
    shot.Frames   = 1800;

    const ShotCamera still = shot.CameraAt( shot.Parameter( 0 ) );
    const ShotCamera last  = shot.CameraAt( shot.Parameter( shot.Frames - 1 ) );

    shot.Play = true;
    EXPECT_FALSE( shot.HasMotion() );
    EXPECT_EQ( shot.CameraAt( shot.Parameter( 0 ) ).Position, still.Position );
    EXPECT_EQ( shot.CameraAt( shot.Parameter( 0 ) ).Forward, still.Forward );
    EXPECT_EQ( shot.CameraAt( shot.Parameter( shot.Frames - 1 ) ).Position, last.Position );
    EXPECT_EQ( shot.CameraAt( shot.Parameter( shot.Frames - 1 ) ).Forward, last.Forward );

    // The converse too: a moving path does not switch the world on.
    ShotOptions moving;
    moving.Output        = "/tmp/out.png";
    moving.HasPositionTo = true;
    moving.PositionTo    = glm::vec3( 30000.0f, 200.0f, 0.0f );
    EXPECT_TRUE( moving.HasMotion() );
    EXPECT_FALSE( moving.PlayActive() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
